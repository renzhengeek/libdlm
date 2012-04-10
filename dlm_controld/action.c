/*
 * Copyright 2004-2011 Red Hat, Inc.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v2 or (at your option) any later version.
 */

#include "dlm_daemon.h"

#include <corosync/corotypes.h>
#include <corosync/cmap.h>

static int dir_members[MAX_NODES];
static int dir_members_count;
static int comms_nodes[MAX_NODES];
static int comms_nodes_count;

#define DLM_SYSFS_DIR "/sys/kernel/dlm"
#define CLUSTER_DIR   "/sys/kernel/config/dlm/cluster"
#define SPACES_DIR    "/sys/kernel/config/dlm/cluster/spaces"
#define COMMS_DIR     "/sys/kernel/config/dlm/cluster/comms"

static int detect_protocol(void)
{
	cmap_handle_t handle;
	char *str = NULL;
	int rv, proto = -1;

	rv = cmap_initialize(&handle);
	if (rv != CS_OK) {
		log_error("cmap_initialize error %d", rv);
		return -1; 
	}

	rv = cmap_get_string(handle, "totem.rrp_mode", &str);
	if (rv != CS_OK)
		goto out;

	log_debug("cmap totem.rrp_mode = '%s'", str);

	if (!strcmp(str, "none"))
		proto = PROTO_TCP;
	else
		proto = PROTO_SCTP;
 out:
	if (str)
		free(str);
	cmap_finalize(handle);
	return proto;
}

static int detect_cluster_name(void)
{
	cmap_handle_t handle;
	char *str = NULL;
	int rv, err = -1;

	rv = cmap_initialize(&handle);
	if (rv != CS_OK) {
		log_error("cmap_initialize error %d", rv);
		return -1; 
	}

	rv = cmap_get_string(handle, "totem.cluster_name", &str);
	if (rv != CS_OK) {
		log_error("cmap_get_string totem.cluster_name error %d", rv);
		goto out;
	} else
		err = 0;

	log_debug("cmap totem.cluster_name = '%s'", str);

	strncpy(cluster_name, str, DLM_LOCKSPACE_LEN);
 out:
	if (str)
		free(str);
	cmap_finalize(handle);
	return err;
}

/* This is for the case where dlm_controld exits/fails, abandoning dlm
   lockspaces in the kernel, and then dlm_controld is restarted.  When
   dlm_controld exits and abandons lockspaces, that node needs to be
   rebooted to clear the uncontrolled lockspaces from the kernel. */

int check_uncontrolled_lockspaces(void)
{
	DIR *d;
	struct dirent *de;
	int count = 0;

	d = opendir(DLM_SYSFS_DIR);
	if (!d)
		return 0;

	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;

		log_error("found uncontrolled lockspace %s", de->d_name);
		count++;
	}
	closedir(d);

	if (count) {
		kick_node_from_cluster(our_nodeid);
		return -1;
	}
	return 0;
}

static int do_sysfs(const char *name, const char *file, char *val)
{
	char fname[512];
	int rv, fd;

	sprintf(fname, "%s/%s/%s", DLM_SYSFS_DIR, name, file);

	fd = open(fname, O_WRONLY);
	if (fd < 0) {
		log_error("open \"%s\" error %d %d", fname, fd, errno);
		return -1;
	}

	log_debug("write \"%s\" to \"%s\"", val, fname);

	rv = do_write(fd, val, strlen(val) + 1);
	close(fd);
	return rv;
}

int set_sysfs_control(char *name, int val)
{
	char buf[32];

	memset(buf, 0, sizeof(buf));
	snprintf(buf, 32, "%d", val);

	return do_sysfs(name, "control", buf);
}

int set_sysfs_event_done(char *name, int val)
{
	char buf[32];

	memset(buf, 0, sizeof(buf));
	snprintf(buf, 32, "%d", val);

	return do_sysfs(name, "event_done", buf);
}

int set_sysfs_id(char *name, uint32_t id)
{
	char buf[32];

	memset(buf, 0, sizeof(buf));
	snprintf(buf, 32, "%u", id);

	return do_sysfs(name, "id", buf);
}

int set_sysfs_nodir(char *name, int val)
{
	char buf[32];

	memset(buf, 0, sizeof(buf));
	snprintf(buf, 32, "%d", val);

	return do_sysfs(name, "nodir", buf);
}

static int update_dir_members(char *name)
{
	char path[PATH_MAX];
	DIR *d;
	struct dirent *de;
	int i = 0;

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX, "%s/%s/nodes", SPACES_DIR, name);

	d = opendir(path);
	if (!d) {
		log_debug("%s: opendir failed: %d", path, errno);
		return -1;
	}

	memset(dir_members, 0, sizeof(dir_members));
	dir_members_count = 0;

	/* FIXME: we should probably read the nodeid in each dir instead */

	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		dir_members[i++] = atoi(de->d_name);
		log_debug("dir_member %d", dir_members[i-1]);
	}
	closedir(d);

	dir_members_count = i;
	return 0;
}

static int id_exists(int id, int count, int *array)
{
	int i;
	for (i = 0; i < count; i++) {
		if (array[i] == id)
			return 1;
	}
	return 0;
}

static int create_path(const char *path)
{
	mode_t old_umask;
	int rv;

	old_umask = umask(0022);
	rv = mkdir(path, 0777);
	if (rv < 0 && errno == EEXIST)
		rv = 0;
	if (rv < 0)
		log_error("%s: mkdir failed: %d", path, errno);
	umask(old_umask);
	return rv;
}

int path_exists(const char *path)
{
	struct stat buf;

	if (stat(path, &buf) < 0) {
		if (errno != ENOENT)
			log_error("%s: stat failed: %d", path, errno);
		return 0;
	}
	return 1;
}

/* The "renew" nodes are those that have left and rejoined since the last
   call to set_members().  We rmdir/mkdir for these nodes so dlm-kernel
   can notice they've left and rejoined. */

int set_configfs_members(struct lockspace *ls, char *name,
			 int new_count, int *new_members,
			 int renew_count, int *renew_members)
{
	char path[PATH_MAX];
	char buf[32];
	int i, w, fd, rv, id, old_count, *old_members;
	int do_renew;

	/*
	 * create lockspace dir if it doesn't exist yet
	 */

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX, "%s/%s", SPACES_DIR, name);

	if (!path_exists(path)) {
		if (create_path(path))
			return -1;
	}

	/*
	 * remove/add lockspace members
	 */

	rv = update_dir_members(name);
	if (rv)
		return rv;

	old_members = dir_members;
	old_count = dir_members_count;

	for (i = 0; i < old_count; i++) {
		id = old_members[i];
		if (id_exists(id, new_count, new_members))
			continue;

		memset(path, 0, PATH_MAX);
		snprintf(path, PATH_MAX, "%s/%s/nodes/%d",
			 SPACES_DIR, name, id);

		log_debug("set_members rmdir \"%s\"", path);

		rv = rmdir(path);
		if (rv) {
			log_error("%s: rmdir failed: %d", path, errno);
			goto out;
		}
	}

	/*
	 * remove lockspace dir after we've removed all the nodes
	 * (when we're shutting down and adding no new nodes)
	 */

	if (!new_count) {
		memset(path, 0, PATH_MAX);
		snprintf(path, PATH_MAX, "%s/%s", SPACES_DIR, name);

		log_debug("set_members lockspace rmdir \"%s\"", path);

		rv = rmdir(path);
		if (rv)
			log_error("%s: rmdir failed: %d", path, errno);
	}

	for (i = 0; i < new_count; i++) {
		id = new_members[i];

		do_renew = 0;

		if (id_exists(id, renew_count, renew_members))
			do_renew = 1;
		else if (id_exists(id, old_count, old_members))
			continue;

		if (!is_cluster_member(id))
			update_cluster();
		/*
		 * create node's dir
		 */

		memset(path, 0, PATH_MAX);
		snprintf(path, PATH_MAX, "%s/%s/nodes/%d",
			 SPACES_DIR, name, id);

		if (do_renew) {
			log_debug("set_members renew rmdir \"%s\"", path);
			rv = rmdir(path);
			if (rv) {
				log_error("%s: renew rmdir failed: %d",
					  path, errno);
				goto out;
			}
		}

		log_debug("set_members mkdir \"%s\"", path);

		rv = create_path(path);
		if (rv)
			goto out;

		/*
		 * set node's nodeid
		 */

		memset(path, 0, PATH_MAX);
		snprintf(path, PATH_MAX, "%s/%s/nodes/%d/nodeid",
			 SPACES_DIR, name, id);

		rv = fd = open(path, O_WRONLY);
		if (rv < 0) {
			log_error("%s: open failed: %d", path, errno);
			goto out;
		}

		memset(buf, 0, 32);
		snprintf(buf, 32, "%d", id);

		rv = do_write(fd, buf, strlen(buf));
		if (rv < 0) {
			log_error("%s: write failed: %d, %s", path, errno, buf);
			close(fd);
			goto out;
		}
		close(fd);

		/*
		 * set node's weight
		 */

		w = get_weight(ls, id);

		memset(path, 0, PATH_MAX);
		snprintf(path, PATH_MAX, "%s/%s/nodes/%d/weight",
			 SPACES_DIR, name, id);

		rv = fd = open(path, O_WRONLY);
		if (rv < 0) {
			log_error("%s: open failed: %d", path, errno);
			goto out;
		}

		memset(buf, 0, 32);
		snprintf(buf, 32, "%d", w);

		rv = do_write(fd, buf, strlen(buf));
		if (rv < 0) {
			log_error("%s: write failed: %d, %s", path, errno, buf);
			close(fd);
			goto out;
		}
		close(fd);
	}

	rv = 0;
 out:
	return rv;
}

#if 0
char *str_ip(char *addr)
{
	static char ip[256];
	struct sockaddr_in *sin = (struct sockaddr_in *) addr;
	memset(ip, 0, sizeof(ip));
	inet_ntop(AF_INET, &sin->sin_addr, ip, 256);
	return ip;
}
#endif

static char *str_ip(char *addr)
{
	static char str_ip_buf[INET6_ADDRSTRLEN];
	struct sockaddr_storage *ss = (struct sockaddr_storage *)addr;
	struct sockaddr_in *sin = (struct sockaddr_in *)addr;
	struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)addr;
	void *saddr;

	if (ss->ss_family == AF_INET6)
		saddr = &sin6->sin6_addr;
	else
		saddr = &sin->sin_addr;

	inet_ntop(ss->ss_family, saddr, str_ip_buf, sizeof(str_ip_buf));
	return str_ip_buf;
}

/* record the nodeids that are currently listed under
   config/dlm/cluster/comms/ so that we can remove all of them */

static int update_comms_nodes(void)
{
	char path[PATH_MAX];
	DIR *d;
	struct dirent *de;
	int i = 0;

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX, COMMS_DIR);

	d = opendir(path);
	if (!d) {
		log_debug("%s: opendir failed: %d", path, errno);
		return -1;
	}

	memset(comms_nodes, 0, sizeof(comms_nodes));
	comms_nodes_count = 0;

	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		comms_nodes[i++] = atoi(de->d_name);
	}
	closedir(d);

	comms_nodes_count = i;
	return 0;
}

/* clear out everything under config/dlm/cluster/comms/ */

static void clear_configfs_comms(void)
{
	char path[PATH_MAX];
	int i, rv;

	rv = update_comms_nodes();
	if (rv < 0)
		return;

	for (i = 0; i < comms_nodes_count; i++) {
		memset(path, 0, PATH_MAX);
		snprintf(path, PATH_MAX, "%s/%d", COMMS_DIR, comms_nodes[i]);

		log_debug("clear_configfs_nodes rmdir \"%s\"", path);

		rv = rmdir(path);
		if (rv)
			log_error("%s: rmdir failed: %d", path, errno);
	}
}

static void clear_configfs_space_nodes(char *name)
{
	char path[PATH_MAX];
	int i, rv;

	rv = update_dir_members(name);
	if (rv < 0)
		return;

	for (i = 0; i < dir_members_count; i++) {
		memset(path, 0, PATH_MAX);
		snprintf(path, PATH_MAX, "%s/%s/nodes/%d",
			 SPACES_DIR, name, dir_members[i]);

		log_debug("clear_configfs_space_nodes rmdir \"%s\"", path);

		rv = rmdir(path);
		if (rv)
			log_error("%s: rmdir failed: %d", path, errno);
	}
}

/* clear out everything under config/dlm/cluster/spaces/ */

static void clear_configfs_spaces(void)
{
	char path[PATH_MAX];
	DIR *d;
	struct dirent *de;
	int rv;

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX, "%s", SPACES_DIR);

	d = opendir(path);
	if (!d) {
		log_debug("%s: opendir failed: %d", path, errno);
		return;
	}

	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;

		clear_configfs_space_nodes(de->d_name);

		memset(path, 0, PATH_MAX);
		snprintf(path, PATH_MAX, "%s/%s", SPACES_DIR, de->d_name);
		
		log_debug("clear_configfs_spaces rmdir \"%s\"", path);

		rv = rmdir(path);
		if (rv)
			log_error("%s: rmdir failed: %d", path, errno);
	}
	closedir(d);
}

static int add_configfs_base(void)
{
	int rv = 0;

	if (!path_exists("/sys/kernel/config")) {
		log_error("No /sys/kernel/config, is configfs loaded?");
		return -1;
	}

	if (!path_exists("/sys/kernel/config/dlm")) {
		log_error("No /sys/kernel/config/dlm, is the dlm loaded?");
		return -1;
	}

	if (!path_exists("/sys/kernel/config/dlm/cluster"))
		rv = create_path("/sys/kernel/config/dlm/cluster");

	return rv;
}

int add_configfs_node(int nodeid, char *addr, int addrlen, int local)
{
	char path[PATH_MAX];
	char padded_addr[sizeof(struct sockaddr_storage)];
	char buf[32];
	int rv, fd;

	log_debug("set_configfs_node %d %s local %d",
		  nodeid, str_ip(addr), local);

	/*
	 * create comm dir for this node
	 */

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX, "%s/%d", COMMS_DIR, nodeid);

	rv = create_path(path);
	if (rv)
		return -1;

	/*
	 * set the nodeid
	 */

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX, "%s/%d/nodeid", COMMS_DIR, nodeid);

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		log_error("%s: open failed: %d", path, errno);
		return -1;
	}

	memset(buf, 0, sizeof(buf));
	snprintf(buf, 32, "%d", nodeid);

	rv = do_write(fd, buf, strlen(buf));
	if (rv < 0) {
		log_error("%s: write failed: %d, %s", path, errno, buf);
		close(fd);
		return -1;
	}
	close(fd);

	/*
	 * set the address
	 */

	memset(padded_addr, 0, sizeof(padded_addr));
	memcpy(padded_addr, addr, addrlen);

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX, "%s/%d/addr", COMMS_DIR, nodeid);

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		log_error("%s: open failed: %d", path, errno);
		return -1;
	}

	rv = do_write(fd, padded_addr, sizeof(struct sockaddr_storage));
	if (rv < 0) {
		log_error("%s: write failed: %d %d", path, errno, rv);
		close(fd);
		return -1;
	}
	close(fd);

	/*
	 * set local
	 */

	if (!local)
		goto out;

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX, "%s/%d/local", COMMS_DIR, nodeid);

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		log_error("%s: open failed: %d", path, errno);
		return -1;
	}

	rv = do_write(fd, (void *)"1", strlen("1"));
	if (rv < 0) {
		log_error("%s: write failed: %d", path, errno);
		close(fd);
		return -1;
	}
	close(fd);
 out:
	return 0;
}

void del_configfs_node(int nodeid)
{
	char path[PATH_MAX];
	int rv;

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX, "%s/%d", COMMS_DIR, nodeid);

	log_debug("del_configfs_node rmdir \"%s\"", path);

	rv = rmdir(path);
	if (rv)
		log_error("%s: rmdir failed: %d", path, errno);
}

/* num may be 0, str won't be NULL */

static int set_configfs_cluster(const char *name, char *str, int num)
{
	char path[PATH_MAX];
	char buf[32];
	char *wbuf;
	int fd, rv;

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX, "%s/%s", CLUSTER_DIR, name);

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		log_error("%s: open failed: %d", path, errno);
		return fd;
	}

	if (str) {
		wbuf = str;
	} else {
		memset(buf, 0, sizeof(buf));
		snprintf(buf, 32, "%d", num);
		wbuf = buf;
	}

	rv = do_write(fd, wbuf, strlen(wbuf));
	if (rv < 0) {
		log_error("%s: write failed: %d", path, errno);
		return rv;
	}
	close(fd);
	log_debug("set %s %s", name, wbuf);
	return 0;
}

#define NET_RMEM_DEFAULT 4194304
#define NET_RMEM_MAX 4194304

static int set_proc_rmem(void)
{
	char path[PATH_MAX];
	char buf[32];
	int fd, rv;

	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX, "/proc/sys/net/core/rmem_default");

	fd = open(path, O_RDWR);
	if (fd < 0) {
		log_error("%s: open failed: %d", path, errno);
		return fd;
	}

	memset(buf, 0, sizeof(buf));

	rv = read(fd, buf, sizeof(buf));
	if (rv < 0) {
		log_error("%s: read failed: %d", path, errno);
		close(fd);
		return rv;
	}

	if (atoi(buf) >= NET_RMEM_DEFAULT) {
		close(fd);
		goto next;
	}

	memset(buf, 0, sizeof(buf));
	snprintf(buf, 32, "%d", NET_RMEM_DEFAULT);

	rv = do_write(fd, buf, strlen(buf));
	if (rv < 0) {
		log_error("%s: write failed: %d", path, errno);
		close(fd);
		return rv;
	}

	close(fd);
	log_debug("set %s %s", path, buf);

 next:
	memset(path, 0, PATH_MAX);
	snprintf(path, PATH_MAX, "/proc/sys/net/core/rmem_max");

	fd = open(path, O_RDWR);
	if (fd < 0) {
		log_error("%s: open failed: %d", path, errno);
		return fd;
	}

	memset(buf, 0, sizeof(buf));

	rv = read(fd, buf, sizeof(buf));
	if (rv < 0) {
		log_error("%s: read failed: %d", path, errno);
		close(fd);
		return rv;
	}

	if (atoi(buf) >= NET_RMEM_MAX) {
		close(fd);
		goto out;
	}

	memset(buf, 0, sizeof(buf));
	snprintf(buf, 32, "%d", NET_RMEM_MAX);

	rv = do_write(fd, buf, strlen(buf));
	if (rv < 0) {
		log_error("%s: write failed: %d", path, errno);
		close(fd);
		return rv;
	}

	close(fd);
	log_debug("set %s %s", path, buf);
 out:
	return 0;
}

void clear_configfs(void)
{
	clear_configfs_comms();
	clear_configfs_spaces();
	rmdir("/sys/kernel/config/dlm/cluster");
}

int setup_configfs_options(void)
{
	char *proto_name;
	int rv, proto_num;

	clear_configfs();

	rv = add_configfs_base();
	if (rv < 0)
		return rv;

	/* the kernel has its own defaults for these values which we
	   don't want to change unless these have been set explicitly
	   on cli or config file */

	if (dlm_options[log_debug_ind].cli_set ||
	    dlm_options[log_debug_ind].file_set)
		set_configfs_cluster("log_debug", NULL, opt(log_debug_ind));

	if (dlm_options[timewarn_ind].cli_set ||
	    dlm_options[timewarn_ind].file_set)
		set_configfs_cluster("timewarn_cs", NULL, opt(timewarn_ind));

	proto_name = opts(protocol_ind);
	proto_num = -1;

	if (!strcasecmp(proto_name, "detect") || !strcmp(proto_name, "2"))
		proto_num = detect_protocol(); /* may be -1 */

	else if (!strcasecmp(proto_name, "tcp") || !strcmp(proto_name, "0"))
		proto_num = PROTO_TCP;

	else if (!strcasecmp(proto_name, "sctp") || !strcmp(proto_name, "1"))
		proto_num = PROTO_SCTP;

	if (proto_num == PROTO_TCP || proto_num == PROTO_SCTP)
		set_configfs_cluster("protocol", NULL, proto_num);

	if (proto_num == PROTO_SCTP)
		set_proc_rmem();

	/* 
	 * set clustername, recover_callbacks
	 *
	 * we can't set our nodeid here, though, it must be set *after*
	 * setup_monitor, because the kernel assumes if the nodeid
	 * is set, but monitor is not opened, that it's an old,
	 * pre-monitor version of dlm_controld and allows it to
	 * go ahead without the monitor being open
	 */

	if (opt(enable_fscontrol_ind)) {
		/* deprecated */
		set_configfs_cluster("recover_callbacks", NULL, 0);
	} else {
		set_configfs_cluster("recover_callbacks", NULL, 1);

		detect_cluster_name();

		if (cluster_name[0]) {
			set_configfs_cluster("cluster_name", cluster_name, 0);
		} else {
			log_error("no cluster name");
			return -1;
		}
	}
	return 0;
}

/* see comment above re why setup_monitor needs to come between
   setup_configfs_options and setup_configfs_members */

int setup_configfs_members(void)
{
	/* add configfs entries for existing nodes */
	update_cluster();
	return 0;
}

static void find_minors(void)
{
	FILE *fl;
	char name[256];
	uint32_t number;
	int found = 0;
	int c;

	control_minor = 0;
	monitor_minor = 0;
	plock_minor = 0;

	if (!(fl = fopen("/proc/misc", "r"))) {
		log_error("/proc/misc fopen failed: %s", strerror(errno));
		return;
	}

	while (!feof(fl)) {
		if (fscanf(fl, "%d %255s\n", &number, &name[0]) == 2) {

			if (!strcmp(name, "dlm-control")) {
				control_minor = number;
				found++;
			} else if (!strcmp(name, "dlm-monitor")) {
				monitor_minor = number;
				found++;
			} else if (!strcmp(name, "dlm_plock")) {
				plock_minor = number;
				found++;
			}

		} else do {
			c = fgetc(fl);
		} while (c != EOF && c != '\n');

		if (found == 3)
			break;
	}
	fclose(fl);

	if (!found)
		log_error("Is dlm missing from kernel? No misc devices found.");
}

static int find_udev_device(const char *path, uint32_t minor)
{
	struct stat st;
	int i;

	for (i = 0; i < 10; i++) {
		if (stat(path, &st) == 0 && minor(st.st_rdev) == minor)
			return 0;
		sleep(1);
	}

	log_error("cannot find device %s with minor %d", path, minor);
	return -1;
}

int setup_misc_devices(void)
{
	int rv;

	find_minors();

	if (control_minor) {
		rv = find_udev_device("/dev/misc/dlm-control", control_minor);
		if (rv < 0)
			return rv;
		log_debug("found /dev/misc/dlm-control minor %u",
			  control_minor);
	}

	if (monitor_minor) {
		rv = find_udev_device("/dev/misc/dlm-monitor", monitor_minor);
		if (rv < 0)
			return rv;
		log_debug("found /dev/misc/dlm-monitor minor %u",
			  monitor_minor);
	}

	if (plock_minor) {
		rv = find_udev_device("/dev/misc/dlm_plock", plock_minor);
		if (rv < 0)
			return rv;
		log_debug("found /dev/misc/dlm_plock minor %u",
			  plock_minor);
	}

	return 0;
}

