/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include "kerncompat.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdbool.h>
#include <strings.h>
#include "kernel-shared/volumes.h"
#include "crypto/hash.h"
#include "common/cpu-utils.h"
#include "common/utils.h"
#include "common/string-utils.h"
#include "common/help.h"
#include "common/box.h"
#include "common/messages.h"
#include "cmds/commands.h"

static const char * const btrfs_cmd_group_usage[] = {
	/*
	 * The main command group is the only one that takes options so this
	 * needs the newlines and manual formatting.
	 */
	"btrfs [global options] <group> [<group>...] <command> [options] [<args>]\n"
	"\n"
	"Global options:\n"
	"  --format <format> if supported, print subcommand output in that format (text, json)\n"
	"  -v|--verbose      increase verbosity of the subcommand\n"
	"  -q|--quiet        print only errors\n"
	"  --log <level>     set log level (default, info, verbose, debug, quiet)\n"
	"  --dry-run         if supported, do not do any active/changing actions\n"
	"\n"
	"Options for the main command only:\n"
	"  --help            print condensed help for all subcommands\n"
	"  --version         print version string and built-in features",
	NULL
};

static const char btrfs_cmd_group_info[] =
	"Use --help as an option for information about a group or command:\n"
	"  btrfs subvolume --help\n"
	"  btrfs subvolume create --help\n"
	"\n"
	"Global options like -q/-v must follow the tool name 'btrfs':\n"
	"  btrfs -q subvolume create ...\n"
	"  btrfs --dry-run subvolume create ..."
	;

static inline const char *skip_prefix(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);
	return strncmp(str, prefix, len) ? NULL : str + len;
}

static int parse_one_token(const char *arg, const struct cmd_group *grp,
			   const struct cmd_struct **cmd_ret)
{
	const struct cmd_struct *abbrev_cmd = NULL, *ambiguous_cmd = NULL;
	int i = 0;

	for (i = 0; grp->commands[i]; i++) {
		const struct cmd_struct *cmd = grp->commands[i];
		const char *rest;

		rest = skip_prefix(arg, cmd->token);
		if (!rest) {
			if (!string_has_prefix(cmd->token, arg)) {
				if (abbrev_cmd) {
					/*
					 * If this is abbreviated, it is
					 * ambiguous. So when there is no
					 * exact match later, we need to
					 * error out.
					 */
					ambiguous_cmd = abbrev_cmd;
				}
				abbrev_cmd = cmd;
			}
			continue;
		}
		if (*rest)
			continue;

		*cmd_ret = cmd;
		return 0;
	}

	if (ambiguous_cmd)
		return -2;

	if (abbrev_cmd) {
		*cmd_ret = abbrev_cmd;
		return 0;
	}

	return -1;
}

static const struct cmd_struct *
parse_command_token(const char *arg, const struct cmd_group *grp)
{
	const struct cmd_struct *cmd = NULL;

	switch(parse_one_token(arg, grp, &cmd)) {
	case -1:
		help_unknown_token(arg, grp);
	case -2:
		help_ambiguous_token(arg, grp);
	}

	return cmd;
}

static void check_command_flags(const struct cmd_struct *cmd)
{
	if (cmd->next)
		return;

	if (!(cmd->flags & bconf.output_format & CMD_FORMAT_MASK)) {
		error("output format %s is unsupported for this command",
			output_format_name(bconf.output_format));
		exit(1);
	}

	if (bconf.dry_run && !(cmd->flags & CMD_DRY_RUN)) {
		error("--dry-run option not supported for %s\n", cmd->token);
		exit(1);
	}
}

static void handle_help_options_next_level(const struct cmd_struct *cmd,
		int argc, char **argv)
{
	if (argc < 2)
		return;

	if (!strcmp(argv[1], "--help")) {
		if (cmd->next) {
			argc--;
			argv++;
			help_command_group(cmd->next, argc, argv);
		} else {
			usage_command(cmd, true, false);
		}

		exit(0);
	}
}

int handle_command_group(const struct cmd_struct *cmd, int argc,
			 char **argv)
{
	const struct cmd_struct *subcmd;

	argc--;
	argv++;
	if (argc < 1) {
		usage_command_group(cmd->next, false, false);
		exit(1);
	}

	subcmd = parse_command_token(argv[0], cmd->next);

	handle_help_options_next_level(subcmd, argc, argv);
	check_command_flags(subcmd);

	fixup_argv0(argv, subcmd->token);
	return cmd_execute(subcmd, argc, argv);
}

static const struct cmd_group btrfs_cmd_group;

static const char * const cmd_help_usage[] = {
	"btrfs help [--full] [--box]",
	"Display help information",
	"",
	OPTLINE("--full", "display detailed help on every command"),
	OPTLINE("--box", "show list of built-in tools (busybox style)"),
	NULL
};

static int cmd_help(const struct cmd_struct *unused, int argc, char **argv)
{
	int i;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--box") == 0) {
#if ENABLE_BOX
			printf("Standalone tools built-in in the busybox style:\n");
			printf("- mkfs.btrfs\n");
			printf("- btrfs-image\n");
			printf("- btrfs-convert\n");
			printf("- btrfstune\n");
			printf("- btrfs-find-root\n");
#else
			printf("No standalone tools built-in in the busybox style\n");
#endif
			exit(0);
		}
	}
	help_command_group(&btrfs_cmd_group, argc, argv);
	return 0;
}

static DEFINE_SIMPLE_COMMAND(help, "help");

static const char * const cmd_version_usage[] = {
	"btrfs version",
	"Display btrfs-progs version",
	NULL
};

static int cmd_version(const struct cmd_struct *unused, int argc, char **argv)
{
	help_builtin_features("");
	return 0;
}
static DEFINE_SIMPLE_COMMAND(version, "version");

static void print_output_formats(FILE *outf)
{
	int i;

	fputs("Options for --format are:", outf);
	for (i = 0; i < ARRAY_SIZE(output_formats); i++)
		fprintf(outf, "%s%s", i ? ", " : " ", output_formats[i].name);
	fputs("\n", outf);
}

static void handle_output_format(const char *format)
{
	int i;
	bool found = false;

	for (i = 0; i < ARRAY_SIZE(output_formats); i++) {
		if (!strcasecmp(format, output_formats[i].name)) {
			bconf.output_format = output_formats[i].value;
			found = true;
			break;
		}
	}

	/* Print error for invalid format */
	if (!found) {
		bconf.output_format = CMD_FORMAT_TEXT;
		fprintf(stderr, "error: invalid output format \"%s\"\n\n",
			format);
		print_output_formats(stderr);
		exit(1);
	}
}

static void handle_log_level(const char *level) {
	if (strcasecmp(level, "default") == 0) {
		bconf.verbose = LOG_DEFAULT;
	} else if (strcasecmp(level, "info") == 0) {
		bconf.verbose = LOG_INFO;
	} else if (strcasecmp(level, "verbose") == 0) {
		bconf.verbose = LOG_VERBOSE;
	} else if (strcasecmp(level, "debug") == 0) {
		bconf.verbose = LOG_DEBUG;
	} else if (strcasecmp(level, "quiet") == 0) {
		bconf.verbose = BTRFS_BCONF_QUIET;
	} else {
		error("unrecognized log level: %s", level);
		exit(1);
	}
}

/*
 * Parse global options, between binary name and first non-option argument
 * after processing all valid options (including those with arguments).
 *
 * Returns index to argv where parsing stopped, optind is reset to 1
 */
static int handle_global_options(int argc, char **argv)
{
	enum { OPT_HELP = 256, OPT_VERSION, OPT_FULL, OPT_FORMAT, OPT_LOG };
	static const struct option long_options[] = {
		{ "help", no_argument, NULL, OPT_HELP },
		{ "version", no_argument, NULL, OPT_VERSION },
		{ "format", required_argument, NULL, OPT_FORMAT },
		{ "full", no_argument, NULL, OPT_FULL },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "log", required_argument, NULL, OPT_LOG },
		{ "param", required_argument, NULL, GETOPT_VAL_PARAM },
		{ "dry-run", no_argument, NULL, GETOPT_VAL_DRY_RUN },
		{ NULL, 0, NULL, 0}
	};
	int shift;

	if (argc == 0)
		return 0;

	opterr = 0;
	while (1) {
		int c;

		c = getopt_long(argc, argv, "+vq", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case OPT_HELP: break;
		case OPT_VERSION: break;
		case OPT_FULL: break;
		case OPT_FORMAT:
			handle_output_format(optarg);
			break;
		case OPT_LOG:
			handle_log_level(optarg);
			break;
		case GETOPT_VAL_PARAM:
			bconf_save_param(optarg);
			break;
		case GETOPT_VAL_DRY_RUN:
			bconf_set_dry_run();
			break;
		case 'v':
			bconf_be_verbose();
			break;
		case 'q':
			bconf_be_quiet();
			break;
		default:
			fprintf(stderr, "Unknown global option: %s\n",
					argv[optind - 1]);
			exit(129);
		}
	}

	shift = optind;
	optind = 1;

	return shift;
}

static void handle_special_globals(int shift, int argc, char **argv)
{
	bool has_help = false;
	bool has_full = false;
	int i;

	for (i = 0; i < shift; i++) {
		if (strcmp(argv[i], "--help") == 0)
			has_help = true;
		else if (strcmp(argv[i], "--full") == 0)
			has_full = true;
	}

	if (has_help) {
		if (has_full)
			usage_command_group(&btrfs_cmd_group, true, false);
		else
			cmd_execute(&cmd_struct_help, argc, argv);
		exit(0);
	}

	for (i = 0; i < shift; i++)
		if (strcmp(argv[i], "--version") == 0) {
			cmd_execute(&cmd_struct_version, argc, argv);
			exit(0);
		}

	for (i = 0; i < shift; i++)
		if (strcmp(argv[i], "--verbose") == 0)
			bconf_be_verbose();

	for (i = 0; i < shift; i++)
		if (strcmp(argv[i], "--quiet") == 0)
			bconf_be_quiet();
}

static const struct cmd_group btrfs_cmd_group = {
	btrfs_cmd_group_usage, btrfs_cmd_group_info, {
		/* Keep subcommands alphabetically sorted */
		&cmd_struct_balance,
		&cmd_struct_check,
		&cmd_struct_device,
		&cmd_struct_filesystem,
		&cmd_struct_inspect,
		&cmd_struct_property,
		&cmd_struct_qgroup,
		&cmd_struct_quota,
		&cmd_struct_receive,
#if EXPERIMENTAL
		&cmd_struct_reflink,
#endif
		&cmd_struct_replace,
		&cmd_struct_rescue,
		&cmd_struct_restore,
		&cmd_struct_scrub,
		&cmd_struct_send,
		&cmd_struct_subvolume,

		/* Help and version stay last */
		&cmd_struct_help,
		&cmd_struct_version,
		NULL
	},
};

int main(int argc, char **argv)
{
	const struct cmd_struct *cmd;
	const char *bname;
	int ret;

	btrfs_config_init();

	if ((bname = strrchr(argv[0], '/')) != NULL)
		bname++;
	else
		bname = argv[0];

	if (!strcmp(bname, "btrfsck")) {
		argv[0] = "check";
#ifdef ENABLE_BOX
	} else if (!strcmp(bname, "mkfs.btrfs")) {
		return mkfs_main(argc, argv);
	} else if (!strcmp(bname, "btrfs-image")) {
		return image_main(argc, argv);
	} else if (!strcmp(bname, "btrfs-convert")) {
		return convert_main(argc, argv);
	} else if (!strcmp(bname, "btrfstune")) {
		return btrfstune_main(argc, argv);
	} else if (!strcmp(bname, "btrfs-find-root")) {
		return find_root_main(argc, argv);
#endif
	} else {
		int shift;

		shift = handle_global_options(argc, argv);
		handle_special_globals(shift, argc, argv);
		while (shift-- > 0) {
			argc--;
			argv++;
		}
		if (argc == 0) {
			usage_command_group_short(&btrfs_cmd_group);
			exit(1);
		}
	}

	cmd = parse_command_token(argv[0], &btrfs_cmd_group);

	handle_help_options_next_level(cmd, argc, argv);
	cpu_detect_flags();
	hash_init_accel();
	fixup_argv0(argv, cmd->token);

	ret = cmd_execute(cmd, argc, argv);

	btrfs_close_all_devices();

	exit(ret);
}
