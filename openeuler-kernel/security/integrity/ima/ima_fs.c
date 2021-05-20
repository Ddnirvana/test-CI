// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2005,2006,2007,2008 IBM Corporation
 *
 * Authors:
 * Kylene Hall <kjhall@us.ibm.com>
 * Reiner Sailer <sailer@us.ibm.com>
 * Mimi Zohar <zohar@us.ibm.com>
 *
 * File: ima_fs.c
 *	implemenents security file system for reporting
 *	current measurement list and IMA statistics
 */

#include <linux/fcntl.h>
#include <linux/kernel_read_file.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/parser.h>
#include <linux/vmalloc.h>
#include <linux/file.h>

#include "ima.h"
#include "ima_digest_list.h"

static DEFINE_MUTEX(ima_write_mutex);

static struct dentry *ima_dir;
static struct dentry *ima_symlink;
static struct dentry *binary_runtime_measurements;
static struct dentry *ascii_runtime_measurements;
static struct dentry *runtime_measurements_count;
static struct dentry *violations;
static struct dentry *ima_policy;
static struct dentry *digests_count;
static struct dentry *digest_list_data;
static struct dentry *digest_list_data_del;

bool ima_canonical_fmt;
static int __init default_canonical_fmt_setup(char *str)
{
#ifdef __BIG_ENDIAN
	ima_canonical_fmt = true;
#endif
	return 1;
}
__setup("ima_canonical_fmt", default_canonical_fmt_setup);

static int valid_policy = 1;

static ssize_t ima_show_htable_value(struct file *filp, char __user *buf,
				     size_t count, loff_t *ppos)
{
	atomic_long_t *val = NULL;
	char tmpbuf[32];	/* greater than largest 'long' string value */
	ssize_t len;

	if (filp->f_path.dentry == violations)
		val = &ima_htable.violations;
	else if (filp->f_path.dentry == runtime_measurements_count)
		val = &ima_htable.len;
#ifdef CONFIG_IMA_DIGEST_LIST
	else if (filp->f_path.dentry == digests_count)
		val = &ima_digests_htable.len;
#endif

	len = scnprintf(tmpbuf, sizeof(tmpbuf), "%li\n", atomic_long_read(val));
	return simple_read_from_buffer(buf, count, ppos, tmpbuf, len);
}

static const struct file_operations ima_htable_value_ops = {
	.read = ima_show_htable_value,
	.llseek = generic_file_llseek,
};

/* returns pointer to hlist_node */
static void *ima_measurements_start(struct seq_file *m, loff_t *pos)
{
	loff_t l = *pos;
	struct ima_queue_entry *qe;

	/* we need a lock since pos could point beyond last element */
	rcu_read_lock();
	list_for_each_entry_rcu(qe, &ima_measurements, later) {
		if (!l--) {
			rcu_read_unlock();
			return qe;
		}
	}
	rcu_read_unlock();
	return NULL;
}

static void *ima_measurements_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct ima_queue_entry *qe = v;

	/* lock protects when reading beyond last element
	 * against concurrent list-extension
	 */
	rcu_read_lock();
	qe = list_entry_rcu(qe->later.next, struct ima_queue_entry, later);
	rcu_read_unlock();
	(*pos)++;

	return (&qe->later == &ima_measurements) ? NULL : qe;
}

static void ima_measurements_stop(struct seq_file *m, void *v)
{
}

void ima_putc(struct seq_file *m, void *data, int datalen)
{
	while (datalen--)
		seq_putc(m, *(char *)data++);
}

/* print format:
 *       32bit-le=pcr#
 *       char[20]=template digest
 *       32bit-le=template name size
 *       char[n]=template name
 *       [eventdata length]
 *       eventdata[n]=template specific data
 */
int ima_measurements_show(struct seq_file *m, void *v)
{
	/* the list never shrinks, so we don't need a lock here */
	struct ima_queue_entry *qe = v;
	struct ima_template_entry *e;
	char *template_name;
	u32 pcr, namelen, template_data_len; /* temporary fields */
	bool is_ima_template = false;
	int i;

	/* get entry */
	e = qe->entry;
	if (e == NULL)
		return -1;

	template_name = (e->template_desc->name[0] != '\0') ?
	    e->template_desc->name : e->template_desc->fmt;

	/*
	 * 1st: PCRIndex
	 * PCR used defaults to the same (config option) in
	 * little-endian format, unless set in policy
	 */
	pcr = !ima_canonical_fmt ? e->pcr : cpu_to_le32(e->pcr);
	ima_putc(m, &pcr, sizeof(e->pcr));

	/* 2nd: template digest */
	ima_putc(m, e->digests[ima_sha1_idx].digest, TPM_DIGEST_SIZE);

	/* 3rd: template name size */
	namelen = !ima_canonical_fmt ? strlen(template_name) :
		cpu_to_le32(strlen(template_name));
	ima_putc(m, &namelen, sizeof(namelen));

	/* 4th:  template name */
	ima_putc(m, template_name, strlen(template_name));

	/* 5th:  template length (except for 'ima' template) */
	if (strcmp(template_name, IMA_TEMPLATE_IMA_NAME) == 0)
		is_ima_template = true;

	if (!is_ima_template) {
		template_data_len = !ima_canonical_fmt ? e->template_data_len :
			cpu_to_le32(e->template_data_len);
		ima_putc(m, &template_data_len, sizeof(e->template_data_len));
	}

	/* 6th:  template specific data */
	for (i = 0; i < e->template_desc->num_fields; i++) {
		enum ima_show_type show = IMA_SHOW_BINARY;
		const struct ima_template_field *field =
			e->template_desc->fields[i];

		if (is_ima_template && strcmp(field->field_id, "d") == 0)
			show = IMA_SHOW_BINARY_NO_FIELD_LEN;
		if (is_ima_template && strcmp(field->field_id, "n") == 0)
			show = IMA_SHOW_BINARY_OLD_STRING_FMT;
		field->field_show(m, show, &e->template_data[i]);
	}
	return 0;
}

static const struct seq_operations ima_measurments_seqops = {
	.start = ima_measurements_start,
	.next = ima_measurements_next,
	.stop = ima_measurements_stop,
	.show = ima_measurements_show
};

static int ima_measurements_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &ima_measurments_seqops);
}

static const struct file_operations ima_measurements_ops = {
	.open = ima_measurements_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

void ima_print_digest(struct seq_file *m, u8 *digest, u32 size)
{
	u32 i;

	for (i = 0; i < size; i++)
		seq_printf(m, "%02x", *(digest + i));
}

/* print in ascii */
static int ima_ascii_measurements_show(struct seq_file *m, void *v)
{
	/* the list never shrinks, so we don't need a lock here */
	struct ima_queue_entry *qe = v;
	struct ima_template_entry *e;
	char *template_name;
	int i;

	/* get entry */
	e = qe->entry;
	if (e == NULL)
		return -1;

	template_name = (e->template_desc->name[0] != '\0') ?
	    e->template_desc->name : e->template_desc->fmt;

	/* 1st: PCR used (config option) */
	seq_printf(m, "%2d ", e->pcr);

	/* 2nd: SHA1 template hash */
	ima_print_digest(m, e->digests[ima_sha1_idx].digest, TPM_DIGEST_SIZE);

	/* 3th:  template name */
	seq_printf(m, " %s", template_name);

	/* 4th:  template specific data */
	for (i = 0; i < e->template_desc->num_fields; i++) {
		seq_puts(m, " ");
		if (e->template_data[i].len == 0)
			continue;

		e->template_desc->fields[i]->field_show(m, IMA_SHOW_ASCII,
							&e->template_data[i]);
	}
	seq_puts(m, "\n");
	return 0;
}

static const struct seq_operations ima_ascii_measurements_seqops = {
	.start = ima_measurements_start,
	.next = ima_measurements_next,
	.stop = ima_measurements_stop,
	.show = ima_ascii_measurements_show
};

static int ima_ascii_measurements_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &ima_ascii_measurements_seqops);
}

static const struct file_operations ima_ascii_measurements_ops = {
	.open = ima_ascii_measurements_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

static ssize_t ima_read_file(char *path, struct dentry *dentry)
{
	void *data = NULL;
	char *datap;
	size_t size;
	struct file *file;
	enum kernel_read_file_id file_id = READING_POLICY;
	int op = DIGEST_LIST_OP_ADD;
	int rc, pathlen = strlen(path);

	char *p;

	/* remove \n */
	datap = path;
	strsep(&datap, "\n");

	if (dentry == digest_list_data || dentry == digest_list_data_del)
		file_id = READING_DIGEST_LIST;

	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file)) {
		pr_err("Unable to open file: %s (%ld)", path, PTR_ERR(file));
		return PTR_ERR(file);
	}

	rc = kernel_read_file(file, 0, &data, INT_MAX, NULL, file_id);
	if (rc < 0) {
		pr_err("Unable to read file: %s (%d)", path, rc);
		fput(file);
		return rc;
	}
	size = rc;
	rc = 0;

	datap = data;
	while (size > 0) {
		if (dentry == ima_policy) {
			p = strsep(&datap, "\n");
			if (p == NULL)
				break;

			pr_debug("rule: %s\n", p);
			rc = ima_parse_add_rule(p);
		} else if (dentry == digest_list_data ||
			   dentry == digest_list_data_del) {
			/*
			 * Disable usage of digest lists if not measured
			 * or appraised.
			 */
			ima_check_measured_appraised(file);

			if (dentry == digest_list_data_del)
				op = DIGEST_LIST_OP_DEL;

			rc = ima_parse_compact_list(size, data, op);
		}

		if (rc < 0)
			break;
		size -= rc;
	}

	vfree(data);
	fput(file);
	if (rc < 0)
		return rc;
	else if (size)
		return -EINVAL;
	else
		return pathlen;
}

static ssize_t ima_write_data(struct file *file, const char __user *buf,
			      size_t datalen, loff_t *ppos)
{
	char *data;
	ssize_t result;
	struct dentry *dentry = file_dentry(file);

	/* No partial writes. */
	result = -EINVAL;
	if (*ppos != 0)
		goto out;

	result = -EFBIG;
	if (datalen > 64 * 1024 * 1024 - 1)
		goto out;

	result = -ENOMEM;
	data = vmalloc(datalen + 1);
	if (!data)
		goto out;

	result = -EFAULT;
	if (copy_from_user(data, buf, datalen) != 0)
		goto out_free;

	data[datalen] = '\0';

	result = mutex_lock_interruptible(&ima_write_mutex);
	if (result < 0)
		goto out_free;

	if (data[0] == '/') {
		result = ima_read_file(data, dentry);
	} else if (dentry == ima_policy) {
		if (ima_appraise & IMA_APPRAISE_POLICY) {
			pr_err("signed policy file (specified "
			       "as an absolute pathname) required\n");
			integrity_audit_msg(AUDIT_INTEGRITY_STATUS, NULL, NULL,
					    "policy_update",
					    "signed policy required", 1, 0);
			result = -EACCES;
		} else {
			result = ima_parse_add_rule(data);
		}
	} else if (dentry == digest_list_data) {
		if (!ima_current_is_parser()) {
			result = -EACCES;
		} else {
			result = ima_parse_compact_list(datalen, data,
							DIGEST_LIST_OP_ADD);
		}
	} else if (dentry == digest_list_data_del) {
		if (!ima_current_is_parser()) {
			result = -EACCES;
		} else {
			result = ima_parse_compact_list(datalen, data,
							DIGEST_LIST_OP_DEL);
		}
	} else {
		pr_err("Unknown data type\n");
		result = -EINVAL;
	}
	mutex_unlock(&ima_write_mutex);
out_free:
	vfree(data);
out:
	if (dentry == ima_policy && result < 0)
		valid_policy = 0;

	return result;
}

enum ima_fs_flags {
	IMA_POLICY_BUSY,
	IMA_DIGEST_LIST_DATA_BUSY,
	IMA_FS_BUSY,
};

static enum ima_fs_flags ima_get_dentry_flag(struct dentry *dentry)
{
	enum ima_fs_flags flag = IMA_FS_BUSY;

	if (dentry == ima_policy)
		flag = IMA_POLICY_BUSY;
	else if (dentry == digest_list_data || dentry == digest_list_data_del)
		flag = IMA_DIGEST_LIST_DATA_BUSY;

	return flag;
}

static unsigned long ima_fs_flags;

#ifdef	CONFIG_IMA_READ_POLICY
static const struct seq_operations ima_policy_seqops = {
		.start = ima_policy_start,
		.next = ima_policy_next,
		.stop = ima_policy_stop,
		.show = ima_policy_show,
};
#endif

/*
 * ima_open_data_upload: sequentialize access to the data upload interface
 */
static int ima_open_data_upload(struct inode *inode, struct file *filp)
{
	struct dentry *dentry = file_dentry(filp);
	const struct seq_operations *seq_ops = NULL;
	enum ima_fs_flags flag = ima_get_dentry_flag(dentry);
	bool read_allowed = false;

	if (dentry == ima_policy) {
#ifdef	CONFIG_IMA_READ_POLICY
		read_allowed = true;
		seq_ops = &ima_policy_seqops;
#endif
	}

	if (!(filp->f_flags & O_WRONLY)) {
		if (!read_allowed)
			return -EACCES;
		if ((filp->f_flags & O_ACCMODE) != O_RDONLY)
			return -EACCES;
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		return seq_open(filp, seq_ops);
	}
	if (test_and_set_bit(flag, &ima_fs_flags))
		return -EBUSY;

	if (dentry == digest_list_data || dentry == digest_list_data_del)
		if (ima_check_current_is_parser())
			ima_set_parser();

	return 0;
}

/*
 * ima_release_data_upload - start using the new measure policy rules.
 *
 * Initially, ima_measure points to the default policy rules, now
 * point to the new policy rules, and remove the securityfs policy file,
 * assuming a valid policy.
 */
static int ima_release_data_upload(struct inode *inode, struct file *file)
{
	struct dentry *dentry = file_dentry(file);
	const char *cause = valid_policy ? "completed" : "failed";
	enum ima_fs_flags flag = ima_get_dentry_flag(dentry);

	if ((file->f_flags & O_ACCMODE) == O_RDONLY)
		return seq_release(inode, file);

	if (dentry == digest_list_data || dentry == digest_list_data_del)
		ima_unset_parser();

	if (dentry != ima_policy) {
		clear_bit(flag, &ima_fs_flags);
		return 0;
	}

	if (valid_policy && ima_check_policy() < 0) {
		cause = "failed";
		valid_policy = 0;
	}

	pr_info("policy update %s\n", cause);
	integrity_audit_msg(AUDIT_INTEGRITY_STATUS, NULL, NULL,
			    "policy_update", cause, !valid_policy, 0);

	if (!valid_policy) {
		ima_delete_rules();
		valid_policy = 1;
		clear_bit(flag, &ima_fs_flags);
		return 0;
	}

	ima_update_policy();
#if !defined(CONFIG_IMA_WRITE_POLICY) && !defined(CONFIG_IMA_READ_POLICY)
	securityfs_remove(ima_policy);
	ima_policy = NULL;
#elif defined(CONFIG_IMA_WRITE_POLICY)
	clear_bit(flag, &ima_fs_flags);
#elif defined(CONFIG_IMA_READ_POLICY)
	inode->i_mode &= ~S_IWUSR;
#endif
	return 0;
}

static const struct file_operations ima_data_upload_ops = {
	.open = ima_open_data_upload,
	.write = ima_write_data,
	.read = seq_read,
	.release = ima_release_data_upload,
	.llseek = generic_file_llseek,
};

int __init ima_fs_init(void)
{
	ima_dir = securityfs_create_dir("ima", integrity_dir);
	if (IS_ERR(ima_dir))
		return -1;

	ima_symlink = securityfs_create_symlink("ima", NULL, "integrity/ima",
						NULL);
	if (IS_ERR(ima_symlink))
		goto out;

	binary_runtime_measurements =
	    securityfs_create_file("binary_runtime_measurements",
				   S_IRUSR | S_IRGRP, ima_dir, NULL,
				   &ima_measurements_ops);
	if (IS_ERR(binary_runtime_measurements))
		goto out;

	ascii_runtime_measurements =
	    securityfs_create_file("ascii_runtime_measurements",
				   S_IRUSR | S_IRGRP, ima_dir, NULL,
				   &ima_ascii_measurements_ops);
	if (IS_ERR(ascii_runtime_measurements))
		goto out;

	runtime_measurements_count =
	    securityfs_create_file("runtime_measurements_count",
				   S_IRUSR | S_IRGRP, ima_dir, NULL,
				   &ima_htable_value_ops);
	if (IS_ERR(runtime_measurements_count))
		goto out;

	violations =
	    securityfs_create_file("violations", S_IRUSR | S_IRGRP,
				   ima_dir, NULL, &ima_htable_value_ops);
	if (IS_ERR(violations))
		goto out;

	ima_policy = securityfs_create_file("policy", POLICY_FILE_FLAGS,
					    ima_dir, NULL,
					    &ima_data_upload_ops);
	if (IS_ERR(ima_policy))
		goto out;

#ifdef CONFIG_IMA_DIGEST_LIST
	digests_count = securityfs_create_file("digests_count",
					       S_IRUSR | S_IRGRP, ima_dir,
					       NULL, &ima_htable_value_ops);
	if (IS_ERR(digests_count))
		goto out;

	digest_list_data = securityfs_create_file("digest_list_data", S_IWUSR,
						  ima_dir, NULL,
						  &ima_data_upload_ops);
	if (IS_ERR(digest_list_data))
		goto out;

	digest_list_data_del = securityfs_create_file("digest_list_data_del",
						      S_IWUSR, ima_dir, NULL,
						      &ima_data_upload_ops);
	if (IS_ERR(digest_list_data_del))
		goto out;
#endif
	return 0;
out:
	securityfs_remove(digest_list_data_del);
	securityfs_remove(digest_list_data);
	securityfs_remove(digests_count);
	securityfs_remove(violations);
	securityfs_remove(runtime_measurements_count);
	securityfs_remove(ascii_runtime_measurements);
	securityfs_remove(binary_runtime_measurements);
	securityfs_remove(ima_symlink);
	securityfs_remove(ima_dir);
	securityfs_remove(ima_policy);
	return -1;
}
