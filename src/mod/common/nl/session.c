#include "mod/common/nl/session.h"

#include "mod/common/log.h"
#include "mod/common/nl/nl_common.h"
#include "mod/common/nl/nl_core.h"
#include "mod/nat64/bib/db.h"

static int session_entry_to_userspace(struct session_entry const *entry,
		void *arg)
{
	struct nlcore_buffer *buffer = (struct nlcore_buffer *) arg;
	struct session_entry_usr entry_usr;
	unsigned long dying_time;

	entry_usr.src6 = entry->src6;
	entry_usr.dst6 = entry->dst6;
	entry_usr.src4 = entry->src4;
	entry_usr.dst4 = entry->dst4;
	entry_usr.state = entry->state;

	dying_time = entry->update_time + entry->timeout;
	entry_usr.dying_time = (dying_time > jiffies)
			? jiffies_to_msecs(dying_time - jiffies)
			: 0;

	return nlbuffer_write(buffer, &entry_usr, sizeof(entry_usr));
}

static int handle_session_display(struct xlator *jool, struct genl_info *info,
		struct request_session *request)
{
	struct nlcore_buffer buffer;
	struct session_foreach_func func = {
			.cb = session_entry_to_userspace,
			.arg = &buffer,
	};
	struct session_foreach_offset offset_struct;
	struct session_foreach_offset *offset = NULL;
	int error;

	log_debug("Sending session table to userspace.");

	error = nlbuffer_init_response(&buffer, info, nlbuffer_response_max_size());
	if (error)
		return nlcore_respond(info, error);

	if (request->foreach.offset_set) {
		offset_struct.offset = request->foreach.offset;
		offset_struct.include_offset = false;
		offset = &offset_struct;
	}

	error = bib_foreach_session(jool, request->l4_proto, &func, offset);
	nlbuffer_set_pending_data(&buffer, error > 0);
	error = (error >= 0)
			? nlbuffer_send(info, &buffer)
			: nlcore_respond(info, error);

	nlbuffer_clean(&buffer);
	return error;
}

int handle_session_config(struct xlator *jool, struct genl_info *info)
{
	struct request_hdr *hdr;
	struct request_session *request;
	int error;

	if (xlat_is_siit()) {
		log_err("SIIT doesn't have session tables.");
		return nlcore_respond(info, -EINVAL);
	}

	hdr = get_jool_hdr(info);
	request = (struct request_session *)(hdr + 1);

	error = validate_request_size(info, sizeof(*request));
	if (error)
		return nlcore_respond(info, error);

	switch (hdr->operation) {
	case OP_FOREACH:
		return handle_session_display(jool, info, request);
	}

	log_err("Unknown operation: %u", hdr->operation);
	return nlcore_respond(info, -EINVAL);
}
