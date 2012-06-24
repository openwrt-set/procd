#include <unistd.h>

#include "procd.h"
#include "service.h"
#include "instance.h"

enum {
	INSTANCE_ATTR_COMMAND,
	INSTANCE_ATTR_ENV,
	INSTANCE_ATTR_DATA,
	__INSTANCE_ATTR_MAX
};

static const struct blobmsg_policy instance_attr[__INSTANCE_ATTR_MAX] = {
	[INSTANCE_ATTR_COMMAND] = { "command", BLOBMSG_TYPE_ARRAY },
	[INSTANCE_ATTR_ENV] = { "env", BLOBMSG_TYPE_TABLE },
	[INSTANCE_ATTR_DATA] = { "data", BLOBMSG_TYPE_TABLE },
};

static void
instance_run(struct service_instance *in)
{
	struct blobmsg_list_node *var;
	struct blob_attr *cur;
	char **argv;
	int argc = 1; /* NULL terminated */
	int rem;

	blobmsg_for_each_attr(cur, in->command, rem)
		argc++;

	blobmsg_list_for_each(&in->env, var)
		putenv(blobmsg_data(var->data));

	argv = alloca(sizeof(char *) * argc);
	argc = 0;

	blobmsg_for_each_attr(cur, in->command, rem)
		argv[argc++] = blobmsg_data(cur);

	argv[argc] = NULL;
	execvp(argv[0], argv);
	exit(127);
}

void
instance_start(struct service_instance *in)
{
	int pid;

	if (in->proc.pending)
		return;

	in->restart = false;
	if (!in->valid)
		return;

	pid = fork();
	if (pid < 0)
		return;

	if (!pid) {
		instance_run(in);
		return;
	}

	in->proc.pid = pid;
	uloop_process_add(&in->proc);
}

static void
instance_timeout(struct uloop_timeout *t)
{
	struct service_instance *in;

	in = container_of(t, struct service_instance, timeout);
	kill(in->proc.pid, SIGKILL);
	uloop_process_delete(&in->proc);
	in->proc.cb(&in->proc, -1);
}

static void
instance_exit(struct uloop_process *p, int ret)
{
	struct service_instance *in;

	in = container_of(p, struct service_instance, proc);
	uloop_timeout_cancel(&in->timeout);
	if (in->restart)
		instance_start(in);
}

void
instance_stop(struct service_instance *in, bool restart)
{
	if (!in->proc.pending)
		return;

	kill(in->proc.pid, SIGTERM);
}

static bool
instance_config_changed(struct service_instance *in, struct service_instance *in_new)
{
	if (!in->valid)
		return true;

	if (!blob_attr_equal(in->command, in_new->command))
		return true;

	if (!blobmsg_list_equal(&in->env, &in_new->env))
		return true;

	if (!blobmsg_list_equal(&in->data, &in_new->data))
		return true;

	return false;
}

static bool
instance_config_parse(struct service_instance *in)
{
	struct blob_attr *tb[__INSTANCE_ATTR_MAX];
	struct blob_attr *cur, *cur2;
	int argc = 0;
	int rem;

	blobmsg_parse(instance_attr, __INSTANCE_ATTR_MAX, tb,
		blobmsg_data(in->config), blobmsg_data_len(in->config));

	cur = tb[INSTANCE_ATTR_COMMAND];
	if (!cur)
		return false;

	if (!blobmsg_check_attr_list(cur, BLOBMSG_TYPE_STRING))
		return false;

	blobmsg_for_each_attr(cur2, cur, rem) {
		argc++;
		break;
	}
	if (!argc)
		return false;

	in->command = cur;

	if ((cur = tb[INSTANCE_ATTR_ENV])) {
		if (!blobmsg_check_attr_list(cur, BLOBMSG_TYPE_STRING))
			return false;
		blobmsg_list_fill(&in->env, blobmsg_data(cur), blobmsg_data_len(cur));
	}

	if ((cur = tb[INSTANCE_ATTR_DATA])) {
		if (!blobmsg_check_attr_list(cur, BLOBMSG_TYPE_STRING))
			return false;
		blobmsg_list_fill(&in->data, blobmsg_data(cur), blobmsg_data_len(cur));
	}

	return true;
}

static void
instance_config_cleanup(struct service_instance *in)
{
	blobmsg_list_free(&in->env);
	blobmsg_list_free(&in->data);
}

static void
instance_config_move(struct service_instance *in, struct service_instance *in_src)
{
	instance_config_cleanup(in);
	blobmsg_list_move(&in->env, &in_src->env);
	blobmsg_list_move(&in->data, &in_src->data);
	in->command = in_src->command;
	in->name = in_src->name;
}

bool
instance_update(struct service_instance *in, struct service_instance *in_new)
{
	bool changed = instance_config_changed(in, in_new);

	in->config = in_new->config;
	if (!changed)
		return false;

	in->restart = true;
	instance_stop(in, true);
	instance_config_move(in, in_new);
	return true;
}

void
instance_free(struct service_instance *in)
{
	uloop_process_delete(&in->proc);
	uloop_timeout_cancel(&in->timeout);
	instance_config_cleanup(in);
	free(in);
}

void
instance_init(struct service_instance *in, struct blob_attr *config)
{
	in->name = blobmsg_name(config);
	in->config = config;
	in->timeout.cb = instance_timeout;
	in->proc.cb = instance_exit;

	blobmsg_list_simple_init(&in->env);
	blobmsg_list_simple_init(&in->data);
	in->valid = instance_config_parse(in);
}

void instance_dump(struct blob_buf *b, struct service_instance *in)
{
	void *i;

	i = blobmsg_open_table(b, in->name);
	blobmsg_add_u8(b, "running", in->proc.pending);
	if (in->proc.pending)
		blobmsg_add_u32(b, "pid", in->proc.pid);
	blobmsg_add_blob(b, in->command);
	blobmsg_close_table(b, i);
}