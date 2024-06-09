#include <assert.h>
#include <stdlib.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/tree/view.h"
#include "log.h"

#include <wlr/types/wlr_output_manager.h>
#include "sway/server.h"

struct cmd_results *cmd_reset_context(int argc, char **argv) {
	struct wlr_output_manager *manager = &server.output_manager;
	wl_signal_emit_mutable(&manager->primary.renderer->events.lost, NULL);

	return cmd_results_new(CMD_SUCCESS, NULL);
}
