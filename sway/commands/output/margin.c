#include <strings.h>
#include "sway/commands.h"
#include "sway/config.h"

struct cmd_results *output_cmd_margin(int argc, char **argv) {
	if (!config->handler_context.output_config) {
		return cmd_results_new(CMD_FAILURE, "Missing output config");
	}
	if (argc < 4) {
		return cmd_results_new(CMD_INVALID, "Missing margin arguments.");
	}

	struct side_gaps margin = {
		.top = atoi(argv[0]),
		.right = atoi(argv[1]),
		.bottom = atoi(argv[2]),
		.left = atoi(argv[3]),
	};

	config->handler_context.output_config->margin = margin;
	config->handler_context.leftovers.argc = argc - 4;
	config->handler_context.leftovers.argv = argv + 4;
	return NULL;
}
