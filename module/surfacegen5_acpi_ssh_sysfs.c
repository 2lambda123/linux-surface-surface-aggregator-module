#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/kernel.h>

#include "surfacegen5_acpi_ssh.h"


static char sg5_ssh_debug_rqst_buf_sysfs[SURFACEGEN5_MAX_RQST_RESPONSE + 1] = { 0 };
static char sg5_ssh_debug_rqst_buf_pld[SURFACEGEN5_MAX_RQST_PAYLOAD] = { 0 };
static char sg5_ssh_debug_rqst_buf_res[SURFACEGEN5_MAX_RQST_RESPONSE] = { 0 };


static ssize_t rqst_read(struct file *f, struct kobject *kobj, struct bin_attribute *attr,
                         char *buf, loff_t offs, size_t count)
{
	if (offs < 0 || count + offs > SURFACEGEN5_MAX_RQST_RESPONSE) {
		return -EINVAL;
	}

	memcpy(buf, sg5_ssh_debug_rqst_buf_sysfs + offs, count);
	return count;
}

static ssize_t rqst_write(struct file *f, struct kobject *kobj, struct bin_attribute *attr,
			  char *buf, loff_t offs, size_t count)
{
	struct surfacegen5_rqst rqst = {};
	struct surfacegen5_buf result = {};
	int status;

	// check basic write constriants
	if (offs != 0 || count > SURFACEGEN5_MAX_RQST_PAYLOAD + 5) {
		return -EINVAL;
	}

	// payload length should be consistent with data provided
	if (buf[4] + 5 != count) {
		return -EINVAL;
	}

	rqst.tc  = buf[0];
	rqst.iid = buf[1];
	rqst.cid = buf[2];
	rqst.snc = buf[3];
	rqst.cdl = buf[4];
	rqst.pld = sg5_ssh_debug_rqst_buf_pld;
	memcpy(sg5_ssh_debug_rqst_buf_pld, buf + 5, count - 5);

	result.cap = SURFACEGEN5_MAX_RQST_RESPONSE;
	result.len = 0;
	result.data = sg5_ssh_debug_rqst_buf_res;

	status = surfacegen5_ec_rqst(&rqst, &result);
	if (status) {
		return status;
	}

	sg5_ssh_debug_rqst_buf_sysfs[0] = result.len;
	memcpy(sg5_ssh_debug_rqst_buf_sysfs + 1, result.data, result.len);
	memset(sg5_ssh_debug_rqst_buf_sysfs + result.len + 1, 0,
	       SURFACEGEN5_MAX_RQST_RESPONSE + 1 - result.len);

	return count;
}

static const BIN_ATTR_RW(rqst, SURFACEGEN5_MAX_RQST_RESPONSE + 1);


int surfacegen5_ssh_sysfs_register(struct device *dev)
{
	return sysfs_create_bin_file(&dev->kobj, &bin_attr_rqst);
}

void surfacegen5_ssh_sysfs_unregister(struct device *dev)
{
	sysfs_remove_bin_file(&dev->kobj, &bin_attr_rqst);
}
