// path: src/u235fs.c
/*
 * u235fs - mountable in-memory pseudo filesystem (Linux kernel module)
 *          modeling a simplified radioactive decay of Uranium-235.
 *
 * Mount:    sudo mount -t u235fs none /mnt/u235fs
 *
 * Files exposed inside the mount:
 *   PARAMS          - simulation parameters (key=value), editable until first RAM
 *   RAM             - control file: its presence == simulation is running
 *   TEMPERATURE     - conditional temperature
 *   DECAYED_NUCLEI  - decayed / remaining nuclei counts
 *   DECAY_RATE      - decay rate per second
 *   ELAPSED_TIME    - real seconds / model years
 *   STATE           - full simulation summary
 *
 * IMPORTANT (floating point):
 *   The decay model uses the exponential law N(t)=N0*exp(-lambda*t). The Linux
 *   kernel has no libm and disables SSE/FP globally, so:
 *     - exp() and string formatting of doubles are implemented here by hand;
 *     - every single floating-point operation is executed ONLY between
 *       kernel_fpu_begin() and kernel_fpu_end();
 *     - the object is compiled with -msse -msse2 (see Makefile).
 *   This makes the module x86_64-only.
 *
 * License: GPL.
 */

#define pr_fmt(fmt) "u235fs: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/kstrtox.h>
#include <linux/version.h>

#include <asm/fpu/api.h>   /* kernel_fpu_begin()/kernel_fpu_end() (x86) */

/* ----------------------------------------------------------------------- */
/* Model constants                                                          */
/* ----------------------------------------------------------------------- */

#define U235FS_MAGIC            0x55323335   /* "U235" */

#define U235_HALF_LIFE_YEARS    704000000.0
#define MODEL_YEARS_PER_SECOND  1000000.0
#define AVOGADRO                6.02214076e23
#define LN2                     0.69314718055994530942

#define BASE_TEMPERATURE        25.0     /* degrees Celsius (conditional)    */
#define MAX_TEMPERATURE         1500.0   /* hard upper clamp                 */
#define TEMP_FACTOR             5.0e-23  /* couples decay rate -> temperature*/
#define NEUTRON_COEFF           1.0e-5   /* small coefficient for neutrons   */
#define NEUTRON_FACTOR_MAX      10.0     /* upper clamp for neutron factor   */

#define RENDER_BUF              1024
#define WRITE_BUF_MAX           4096

/* ----------------------------------------------------------------------- */
/* File identification                                                      */
/* ----------------------------------------------------------------------- */

enum u235_file {
	U235_ROOT = 0,
	U235_PARAMS,
	U235_RAM,
	U235_TEMPERATURE,
	U235_DECAYED,
	U235_RATE,
	U235_ELAPSED,
	U235_STATE,
};

/* ----------------------------------------------------------------------- */
/* Simulation state (one per mounted super block)                           */
/* ----------------------------------------------------------------------- */

struct u235_sim {
	struct mutex		lock;
	struct delayed_work	dwork;
	struct super_block	*sb;

	/* parameters (from PARAMS) */
	long	mass;			/* grams                          */
	long	volume;			/* observed volume (>= 1)         */
	long	freq_milli;		/* initial_frequency * 1000       */
	long	impurities;
	long	external_neutrons;
	long	neutron_speed;

	bool	params_locked;		/* true after first RAM creation  */
	bool	started;		/* true after first RAM creation  */
	bool	running;		/* currently ticking              */
	bool	ram_present;		/* RAM file currently exists      */

	u64	real_seconds;		/* model ticks elapsed            */

	/*
	 * Computed values. These doubles are touched ONLY inside
	 * kernel_fpu_begin()/kernel_fpu_end() regions (u235fs_recompute()).
	 */
	double	N0;
	double	remaining;
	double	decayed;
	double	decay_rate;
	double	temperature;

	/* Pre-formatted strings (read path copies these, no FP needed). */
	char	s_remaining[32];
	char	s_decayed[32];
	char	s_rate[32];
	char	s_temp[24];
};

/* forward declarations of ops tables (used by u235fs_make_inode) */
static const struct inode_operations	u235fs_dir_inode_ops;
static const struct inode_operations	u235fs_file_inode_ops;
static const struct file_operations	u235fs_file_ops;

/* ======================================================================= */
/* Floating-point helpers - CALL ONLY INSIDE kernel_fpu_begin()/end()       */
/* ======================================================================= */

/* exp(x) via range reduction + Taylor series; valid for any finite x. */
static double k_exp(double x)
{
	int neg = 0;
	int k = 0;
	double sum, term;
	int i;

	if (x == 0.0)
		return 1.0;
	if (x < 0.0) {
		neg = 1;
		x = -x;
	}

	/* reduce argument so that x < 0.5 (Taylor converges quickly) */
	while (x > 0.5) {
		x /= 2.0;
		k++;
	}

	sum = 1.0;
	term = 1.0;
	for (i = 1; i < 24; i++) {
		term *= x / (double)i;
		sum += term;
		if (term < 1e-18)
			break;
	}

	/* square back k times: exp(x) = (exp(x/2^k))^(2^k) */
	while (k-- > 0)
		sum *= sum;

	if (neg)
		sum = 1.0 / sum;
	return sum;
}

/* Parse a decimal/scientific string into a double (e.g. "2.5", "1e3"). */
static double parse_double(const char *s)
{
	double result = 0.0, frac = 0.0, scale = 1.0;
	int sign = 1, exp_sign = 1, exp_val = 0;
	int has_exp = 0;
	int i = 0;

	while (s[i] == ' ' || s[i] == '\t')
		i++;
	if (s[i] == '-') { sign = -1; i++; }
	else if (s[i] == '+') { i++; }

	while (s[i] >= '0' && s[i] <= '9') {
		result = result * 10.0 + (double)(s[i] - '0');
		i++;
	}
	if (s[i] == '.') {
		i++;
		while (s[i] >= '0' && s[i] <= '9') {
			frac = frac * 10.0 + (double)(s[i] - '0');
			scale *= 10.0;
			i++;
		}
		result += frac / scale;
	}
	if (s[i] == 'e' || s[i] == 'E') {
		i++;
		has_exp = 1;
		if (s[i] == '-') { exp_sign = -1; i++; }
		else if (s[i] == '+') { i++; }
		while (s[i] >= '0' && s[i] <= '9') {
			exp_val = exp_val * 10 + (s[i] - '0');
			i++;
		}
	}
	if (has_exp) {
		double p = 1.0;
		int j;

		for (j = 0; j < exp_val; j++)
			p *= 10.0;
		if (exp_sign < 0)
			result /= p;
		else
			result *= p;
	}
	return (double)sign * result;
}

/* Format value in scientific notation: "m.mmme+NN" (e.g. "2.551e+24"). */
static void fmt_sci(char *buf, size_t n, double v)
{
	int neg = 0, e = 0;
	double m;
	long ip;
	int fp;

	if (v == 0.0) {
		snprintf(buf, n, "0.000e+00");
		return;
	}
	if (v < 0.0) { neg = 1; v = -v; }

	m = v;
	while (m >= 10.0) { m /= 10.0; e++; }
	while (m < 1.0)   { m *= 10.0; e--; }

	ip = (long)m;					/* single digit 1..9 */
	fp = (int)((m - (double)ip) * 1000.0 + 0.5);	/* 3 decimals        */
	if (fp >= 1000) {
		fp -= 1000;
		ip += 1;
		if (ip >= 10) { ip = 1; e++; }
	}

	snprintf(buf, n, "%s%ld.%03de%c%02d",
		 neg ? "-" : "", ip, fp,
		 (e < 0) ? '-' : '+', (e < 0) ? -e : e);
}

/* Format value with exactly two decimals: "38.42". */
static void fmt_fixed2(char *buf, size_t n, double v)
{
	long ip;
	int fp;
	double r;
	int neg = 0;

	if (v < 0.0) { neg = 1; v = -v; }
	ip = (long)v;
	r = v - (double)ip;
	fp = (int)(r * 100.0 + 0.5);
	if (fp >= 100) { fp -= 100; ip += 1; }

	snprintf(buf, n, "%s%ld.%02d", neg ? "-" : "", ip, fp);
}

/* ======================================================================= */
/* Integer-only formatting (no FPU)                                         */
/* ======================================================================= */

/* Format frequency stored as milli-units (1000 -> "1.0", 2500 -> "2.5"). */
static void fmt_freq_milli(char *buf, size_t n, long m)
{
	long ip;
	int fp, neg = 0;

	if (m < 0) { neg = 1; m = -m; }
	ip = m / 1000;
	fp = (int)(m % 1000);

	if (fp % 100 == 0)
		snprintf(buf, n, "%s%ld.%01d", neg ? "-" : "", ip, fp / 100);
	else if (fp % 10 == 0)
		snprintf(buf, n, "%s%ld.%02d", neg ? "-" : "", ip, fp / 10);
	else
		snprintf(buf, n, "%s%ld.%03d", neg ? "-" : "", ip, fp);
}

/* ======================================================================= */
/* Core model: recompute one tick. Caller must hold sim->lock.              */
/* ======================================================================= */

static void u235fs_recompute(struct u235_sim *sim)
{
	long mass	= sim->mass;
	long volume	= (sim->volume < 1) ? 1 : sim->volume;
	long impurities = sim->impurities;
	long ext_n	= sim->external_neutrons;
	long nspeed	= sim->neutron_speed;
	long freq_milli = sim->freq_milli;
	u64  secs	= sim->real_seconds;

	kernel_fpu_begin();
	{
		double freq = (double)freq_milli / 1000.0;
		double moles, N0, remaining, decayed, rate, temp;
		double lambda, lambda_eff, model_years;
		double impurity_factor, neutron_factor, density_factor;
		double prev_remaining = sim->remaining;

		if (freq < 0.0)
			freq = 0.0;

		/* N0 from mass:  moles = mass/235 ; N0 = moles * Avogadro */
		moles = (double)mass / 235.0;
		N0 = moles * AVOGADRO;
		if (N0 < 0.0)
			N0 = 0.0;

		/* base decay constant */
		lambda = LN2 / U235_HALF_LIFE_YEARS;

		/* impurities reduce effective activity */
		impurity_factor = 1.0 / (1.0 + (double)impurities * 0.05);
		if (impurity_factor <= 0.0)
			impurity_factor = 1e-6;

		/* external neutrons add induced activity (clamped) */
		neutron_factor = 1.0 +
			(double)ext_n * (double)nspeed * NEUTRON_COEFF;
		if (neutron_factor < 1.0)
			neutron_factor = 1.0;
		if (neutron_factor > NEUTRON_FACTOR_MAX)
			neutron_factor = NEUTRON_FACTOR_MAX;

		/* effective decay constant */
		lambda_eff = lambda * freq * impurity_factor * neutron_factor;
		if (lambda_eff < 0.0)
			lambda_eff = 0.0;

		/* scaled model time */
		model_years = (double)secs * MODEL_YEARS_PER_SECOND;

		/* exponential decay law */
		remaining = N0 * k_exp(-lambda_eff * model_years);
		if (remaining < 0.0)
			remaining = 0.0;
		if (remaining > N0)
			remaining = N0;

		decayed = N0 - remaining;
		if (decayed < 0.0)
			decayed = 0.0;

		/* decay rate = nuclei decayed during this 1 s tick */
		if (secs == 0) {
			rate = 0.0;
		} else {
			rate = prev_remaining - remaining;
			if (rate < 0.0)
				rate = 0.0;
		}

		/* smaller volume -> higher conditional density/activity */
		density_factor = (double)mass / (double)volume;
		if (density_factor < 0.0)
			density_factor = 0.0;

		temp = BASE_TEMPERATURE + rate * TEMP_FACTOR * density_factor;
		if (temp < BASE_TEMPERATURE)
			temp = BASE_TEMPERATURE;
		if (temp > MAX_TEMPERATURE)
			temp = MAX_TEMPERATURE;

		sim->N0		= N0;
		sim->remaining	= remaining;
		sim->decayed	= decayed;
		sim->decay_rate	= rate;
		sim->temperature = temp;

		fmt_sci(sim->s_remaining, sizeof(sim->s_remaining), remaining);
		fmt_sci(sim->s_decayed,   sizeof(sim->s_decayed),   decayed);
		fmt_sci(sim->s_rate,      sizeof(sim->s_rate),      rate);
		fmt_fixed2(sim->s_temp,   sizeof(sim->s_temp),      temp);
	}
	kernel_fpu_end();
}

/* ======================================================================= */
/* Periodic update (delayed work, fires once per second)                   */
/* ======================================================================= */

static void u235fs_tick(struct work_struct *w)
{
	struct u235_sim *sim =
		container_of(to_delayed_work(w), struct u235_sim, dwork);
	bool reschedule = false;

	mutex_lock(&sim->lock);
	if (sim->running) {
		sim->real_seconds++;
		u235fs_recompute(sim);
		reschedule = true;
	}
	mutex_unlock(&sim->lock);

	if (reschedule)
		schedule_delayed_work(&sim->dwork, HZ);
}

/* ======================================================================= */
/* Inode/dentry helpers                                                     */
/* ======================================================================= */

static inline void u235fs_set_times(struct inode *inode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	simple_inode_init_ts(inode);
#else
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
#endif
}

static struct inode *u235fs_make_inode(struct super_block *sb, umode_t mode,
				       enum u235_file type)
{
	struct inode *inode = new_inode(sb);

	if (!inode)
		return NULL;

	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode->i_private = (void *)(long)type;
	u235fs_set_times(inode);

	if (S_ISDIR(mode)) {
		inode->i_op = &u235fs_dir_inode_ops;
		inode->i_fop = &simple_dir_operations;
		inc_nlink(inode);
	} else {
		inode->i_op = &u235fs_file_inode_ops;
		inode->i_fop = &u235fs_file_ops;
		inode->i_size = 0;
	}
	return inode;
}

/* Create and hash a file dentry as a child of @parent (pins it in dcache). */
static struct dentry *u235fs_add_file(struct super_block *sb,
				      struct dentry *parent,
				      const char *name,
				      enum u235_file type, umode_t mode)
{
	struct dentry *d;
	struct inode *inode;

	d = d_alloc_name(parent, name);
	if (!d)
		return NULL;

	inode = u235fs_make_inode(sb, S_IFREG | mode, type);
	if (!inode) {
		dput(d);
		return NULL;
	}

	d_add(d, inode);	/* keep the ref: pins the file for fs lifetime */
	return d;
}

static int u235fs_add_state_files(struct super_block *sb, struct dentry *parent)
{
	static const struct {
		const char	*name;
		enum u235_file	type;
	} files[] = {
		{ "TEMPERATURE",    U235_TEMPERATURE },
		{ "DECAYED_NUCLEI", U235_DECAYED     },
		{ "DECAY_RATE",     U235_RATE        },
		{ "ELAPSED_TIME",   U235_ELAPSED     },
		{ "STATE",          U235_STATE       },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(files); i++) {
		if (!u235fs_add_file(sb, parent, files[i].name,
				     files[i].type, 0444)) {
			pr_err("failed to create %s\n", files[i].name);
			return -ENOMEM;
		}
	}
	return 0;
}

/* ======================================================================= */
/* File read: content is generated on demand from current sim state        */
/* ======================================================================= */

static ssize_t u235fs_read(struct file *file, char __user *ubuf,
			   size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct u235_sim *sim = inode->i_sb->s_fs_info;
	enum u235_file type = (enum u235_file)(long)inode->i_private;
	char freq[24];
	char *buf;
	int n = 0;
	ssize_t ret;

	buf = kmalloc(RENDER_BUF, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&sim->lock);
	switch (type) {
	case U235_PARAMS:
		fmt_freq_milli(freq, sizeof(freq), sim->freq_milli);
		n = scnprintf(buf, RENDER_BUF,
			"mass=%ld\nvolume=%ld\ninitial_frequency=%s\n"
			"impurities=%ld\nexternal_neutrons=%ld\nneutron_speed=%ld\n",
			sim->mass, sim->volume, freq,
			sim->impurities, sim->external_neutrons,
			sim->neutron_speed);
		break;
	case U235_RAM:
		n = scnprintf(buf, RENDER_BUF,
			"simulation_control_file\nram_present=%d\nstate=%s\n",
			sim->ram_present ? 1 : 0,
			sim->running ? "running" : "paused");
		break;
	case U235_ELAPSED:
		n = scnprintf(buf, RENDER_BUF,
			"real_seconds=%llu\nmodel_years=%llu\n",
			(unsigned long long)sim->real_seconds,
			(unsigned long long)(sim->real_seconds * 1000000ULL));
		break;
	case U235_DECAYED:
		n = scnprintf(buf, RENDER_BUF,
			"decayed_nuclei=%s\nremaining_nuclei=%s\n",
			sim->s_decayed, sim->s_remaining);
		break;
	case U235_RATE:
		n = scnprintf(buf, RENDER_BUF,
			"decay_rate_per_second=%s\n", sim->s_rate);
		break;
	case U235_TEMPERATURE:
		n = scnprintf(buf, RENDER_BUF,
			"temperature_celsius=%s\n", sim->s_temp);
		break;
	case U235_STATE:
		n = scnprintf(buf, RENDER_BUF,
			"state=%s\nram_present=%d\nparams_locked=%d\n"
			"real_elapsed_seconds=%llu\nmodel_time_years=%llu\n"
			"mass=%ld\nvolume=%ld\n"
			"remaining_nuclei=%s\ndecayed_nuclei=%s\n"
			"decay_rate_per_second=%s\ntemperature_celsius=%s\n",
			sim->running ? "running" : "paused",
			sim->ram_present ? 1 : 0,
			sim->params_locked ? 1 : 0,
			(unsigned long long)sim->real_seconds,
			(unsigned long long)(sim->real_seconds * 1000000ULL),
			sim->mass, sim->volume,
			sim->s_remaining, sim->s_decayed,
			sim->s_rate, sim->s_temp);
		break;
	default:
		n = 0;
		break;
	}
	mutex_unlock(&sim->lock);

	ret = simple_read_from_buffer(ubuf, len, ppos, buf, n);
	kfree(buf);
	return ret;
}

/* ======================================================================= */
/* File write: only PARAMS, only before the first RAM creation             */
/* ======================================================================= */

static ssize_t u235fs_write(struct file *file, const char __user *ubuf,
			    size_t len, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct u235_sim *sim = inode->i_sb->s_fs_info;
	enum u235_file type = (enum u235_file)(long)inode->i_private;
	char *kbuf, *p, *line;
	char freqbuf[32];
	bool have_freq = false;

	if (type != U235_PARAMS)
		return -EPERM;
	if (len == 0)
		return 0;
	if (len > WRITE_BUF_MAX)
		len = WRITE_BUF_MAX;

	kbuf = kmalloc(len + 1, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (copy_from_user(kbuf, ubuf, len)) {
		kfree(kbuf);
		return -EFAULT;
	}
	kbuf[len] = '\0';

	mutex_lock(&sim->lock);
	if (sim->params_locked) {
		mutex_unlock(&sim->lock);
		kfree(kbuf);
		pr_info("PARAMS is locked (simulation already started)\n");
		return -EPERM;
	}

	p = kbuf;
	while ((line = strsep(&p, "\n")) != NULL) {
		char *key, *val;
		long v;

		line = strim(line);
		if (*line == '\0' || *line == '#')
			continue;

		val = line;
		key = strsep(&val, "=");
		if (!val)
			continue;
		key = strim(key);
		val = strim(val);

		if (!strcmp(key, "mass")) {
			if (!kstrtol(val, 10, &v))
				sim->mass = (v < 0) ? 0 : v;
		} else if (!strcmp(key, "volume")) {
			if (!kstrtol(val, 10, &v))
				sim->volume = (v < 1) ? 1 : v;
		} else if (!strcmp(key, "impurities")) {
			if (!kstrtol(val, 10, &v))
				sim->impurities = (v < 0) ? 0 : v;
		} else if (!strcmp(key, "external_neutrons")) {
			if (!kstrtol(val, 10, &v))
				sim->external_neutrons = (v < 0) ? 0 : v;
		} else if (!strcmp(key, "neutron_speed")) {
			if (!kstrtol(val, 10, &v))
				sim->neutron_speed = (v < 0) ? 0 : v;
		} else if (!strcmp(key, "initial_frequency")) {
			strscpy(freqbuf, val, sizeof(freqbuf));
			have_freq = true;
		}
	}

	if (have_freq) {
		long milli;

		kernel_fpu_begin();
		{
			double f = parse_double(freqbuf);

			if (f < 0.0)
				f = 0.0;
			milli = (long)(f * 1000.0 + 0.5);
		}
		kernel_fpu_end();
		sim->freq_milli = milli;
	}
	mutex_unlock(&sim->lock);

	kfree(kbuf);
	return len;
}

/* ======================================================================= */
/* Directory inode ops: create RAM, unlink RAM                              */
/* ======================================================================= */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int u235fs_create(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode, bool excl)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
static int u235fs_create(struct user_namespace *mnt_userns, struct inode *dir,
			 struct dentry *dentry, umode_t mode, bool excl)
#else
static int u235fs_create(struct inode *dir, struct dentry *dentry,
			 umode_t mode, bool excl)
#endif
{
	struct super_block *sb = dir->i_sb;
	struct u235_sim *sim = sb->s_fs_info;
	struct inode *inode;
	int err = 0;

	if (strcmp(dentry->d_name.name, "RAM") != 0) {
		pr_warn("only 'RAM' may be created in u235fs\n");
		return -EPERM;
	}

	inode = u235fs_make_inode(sb, S_IFREG | 0444, U235_RAM);
	if (!inode)
		return -ENOMEM;

	d_instantiate(dentry, inode);
	dget(dentry);	/* pin the RAM dentry (released on unlink) */

	mutex_lock(&sim->lock);
	if (!sim->started) {
		/* First start: lock PARAMS, init state, create state files. */
		sim->started = true;
		sim->params_locked = true;
		sim->real_seconds = 0;
		u235fs_recompute(sim);		/* t = 0 snapshot          */
		err = u235fs_add_state_files(sb, dentry->d_parent);
	}
	sim->ram_present = true;
	sim->running = true;
	mutex_unlock(&sim->lock);

	schedule_delayed_work(&sim->dwork, HZ);
	pr_info("simulation running\n");
	return err;
}

static int u235fs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct u235_sim *sim = dir->i_sb->s_fs_info;

	if (strcmp(dentry->d_name.name, "RAM") != 0) {
		pr_warn("only 'RAM' may be removed in u235fs\n");
		return -EPERM;
	}

	/* Pause: stop ticking but keep all values in memory. */
	mutex_lock(&sim->lock);
	sim->running = false;
	sim->ram_present = false;
	mutex_unlock(&sim->lock);

	cancel_delayed_work_sync(&sim->dwork);
	pr_info("simulation paused (RAM removed)\n");

	return simple_unlink(dir, dentry);
}

/* ======================================================================= */
/* Ops tables                                                               */
/* ======================================================================= */

static const struct inode_operations u235fs_dir_inode_ops = {
	.lookup	= simple_lookup,
	.create	= u235fs_create,
	.unlink	= u235fs_unlink,
};

static const struct inode_operations u235fs_file_inode_ops = {
	.setattr = simple_setattr,
	.getattr = simple_getattr,
};

static const struct file_operations u235fs_file_ops = {
	.owner		= THIS_MODULE,
	.open		= simple_open,
	.read		= u235fs_read,
	.write		= u235fs_write,
	.llseek		= default_llseek,
};

static const struct super_operations u235fs_super_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
};

/* ======================================================================= */
/* Super block lifecycle                                                    */
/* ======================================================================= */

static int u235fs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct u235_sim *sim;
	struct inode *root;

	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_magic = U235FS_MAGIC;
	sb->s_op = &u235fs_super_ops;
	sb->s_time_gran = 1;

	sim = kzalloc(sizeof(*sim), GFP_KERNEL);
	if (!sim)
		return -ENOMEM;

	mutex_init(&sim->lock);
	INIT_DELAYED_WORK(&sim->dwork, u235fs_tick);
	sim->sb = sb;

	/* default parameters */
	sim->mass		= 1000;
	sim->volume		= 10;
	sim->freq_milli		= 1000;	/* initial_frequency = 1.0 */
	sim->impurities		= 0;
	sim->external_neutrons	= 0;
	sim->neutron_speed	= 2200;
	sim->params_locked	= false;
	sim->started		= false;
	sim->running		= false;
	sim->ram_present	= false;
	sim->real_seconds	= 0;

	sb->s_fs_info = sim;

	root = u235fs_make_inode(sb, S_IFDIR | 0755, U235_ROOT);
	if (!root)
		return -ENOMEM;
	set_nlink(root, 2);

	sb->s_root = d_make_root(root);
	if (!sb->s_root)
		return -ENOMEM;

	if (!u235fs_add_file(sb, sb->s_root, "PARAMS", U235_PARAMS, 0644))
		return -ENOMEM;

	pr_info("mounted (mass=%ld volume=%ld)\n", sim->mass, sim->volume);
	return 0;
}

static struct dentry *u235fs_mount(struct file_system_type *fst, int flags,
				   const char *dev, void *data)
{
	return mount_nodev(fst, flags, data, u235fs_fill_super);
}

static void u235fs_kill_sb(struct super_block *sb)
{
	struct u235_sim *sim = sb->s_fs_info;

	if (sim)
		cancel_delayed_work_sync(&sim->dwork);

	kill_litter_super(sb);	/* drops all pinned dentries/inodes */
	kfree(sim);		/* kfree(NULL) is safe */
}

static struct file_system_type u235fs_type = {
	.owner		= THIS_MODULE,
	.name		= "u235fs",
	.mount		= u235fs_mount,
	.kill_sb	= u235fs_kill_sb,
	.fs_flags	= 0,
};

/* ======================================================================= */
/* Module init/exit                                                         */
/* ======================================================================= */

static int __init u235fs_init(void)
{
	int ret = register_filesystem(&u235fs_type);

	if (ret)
		pr_err("register_filesystem failed: %d\n", ret);
	else
		pr_info("registered filesystem 'u235fs'\n");
	return ret;
}

static void __exit u235fs_exit(void)
{
	unregister_filesystem(&u235fs_type);
	pr_info("unregistered filesystem 'u235fs'\n");
}

module_init(u235fs_init);
module_exit(u235fs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("u235fs educational project");
MODULE_DESCRIPTION("Pseudo filesystem modeling U-235 radioactive decay");
MODULE_VERSION("1.0");
