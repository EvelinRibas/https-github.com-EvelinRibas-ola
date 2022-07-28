/*
 * Example implementation for the Git filter protocol version 2
 * See Documentation/gitattributes.txt, section "Filter Protocol"
 *
 * Usage: test-tool rot13-filter [--always-delay] <log path> <capabilities>
 *
 * Log path defines a debug log file that the script writes to. The
 * subsequent arguments define a list of supported protocol capabilities
 * ("clean", "smudge", etc).
 *
 * When --always-delay is given all pathnames with the "can-delay" flag
 * that don't appear on the list bellow are delayed with a count of 1
 * (see more below).
 *
 * This implementation supports special test cases:
 * (1) If data with the pathname "clean-write-fail.r" is processed with
 *     a "clean" operation then the write operation will die.
 * (2) If data with the pathname "smudge-write-fail.r" is processed with
 *     a "smudge" operation then the write operation will die.
 * (3) If data with the pathname "error.r" is processed with any
 *     operation then the filter signals that it cannot or does not want
 *     to process the file.
 * (4) If data with the pathname "abort.r" is processed with any
 *     operation then the filter signals that it cannot or does not want
 *     to process the file and any file after that is processed with the
 *     same command.
 * (5) If data with a pathname that is a key in the delay hash is
 *     requested (e.g. "test-delay10.a") then the filter responds with
 *     a "delay" status and sets the "requested" field in the delay hash.
 *     The filter will signal the availability of this object after
 *     "count" (field in delay hash) "list_available_blobs" commands.
 * (6) If data with the pathname "missing-delay.a" is processed that the
 *     filter will drop the path from the "list_available_blobs" response.
 * (7) If data with the pathname "invalid-delay.a" is processed that the
 *     filter will add the path "unfiltered" which was not delayed before
 *     to the "list_available_blobs" response.
 */

#include "test-tool.h"
#include "pkt-line.h"
#include "string-list.h"
#include "strmap.h"

static FILE *logfile;
static int always_delay;
static struct strmap delay = STRMAP_INIT;
static struct string_list requested_caps = STRING_LIST_INIT_NODUP;

static int has_capability(const char *cap)
{
	return unsorted_string_list_has_string(&requested_caps, cap);
}

static char *rot13(char *str)
{
	char *c;
	for (c = str; *c; c++) {
		if (*c >= 'a' && *c <= 'z')
			*c = 'a' + (*c - 'a' + 13) % 26;
		else if (*c >= 'A' && *c <= 'Z')
			*c = 'A' + (*c - 'A' + 13) % 26;
	}
	return str;
}

static char *skip_key_dup(const char *buf, size_t size, const char *key)
{
	struct strbuf keybuf = STRBUF_INIT;
	strbuf_addf(&keybuf, "%s=", key);
	if (!skip_prefix_mem(buf, size, keybuf.buf, &buf, &size) || !size)
		die("bad %s: '%s'", key, xstrndup(buf, size));
	strbuf_release(&keybuf);
	return xstrndup(buf, size);
}

/*
 * Read a text packet, expecting that it is in the form "key=value" for
 * the given key. An EOF does not trigger any error and is reported
 * back to the caller with NULL. Die if the "key" part of "key=value" does
 * not match the given key, or the value part is empty.
 */
static char *packet_key_val_read(const char *key)
{
	int size;
	char *buf;
	if (packet_read_line_gently(0, &size, &buf) < 0)
		return NULL;
	return skip_key_dup(buf, size, key);
}

static void packet_read_capabilities(struct string_list *caps)
{
	while (1) {
		int size;
		char *buf = packet_read_line(0, &size);
		if (!buf)
			break;
		string_list_append_nodup(caps,
					 skip_key_dup(buf, size, "capability"));
	}
}

/* Read remote capabilities and check them against capabilities we require */
static void packet_read_and_check_capabilities(struct string_list *remote_caps,
					       struct string_list *required_caps)
{
	struct string_list_item *item;
	packet_read_capabilities(remote_caps);
	for_each_string_list_item(item, required_caps) {
		if (!unsorted_string_list_has_string(remote_caps, item->string)) {
			die("required '%s' capability not available from remote",
			    item->string);
		}
	}
}

/*
 * Check our capabilities we want to advertise against the remote ones
 * and then advertise our capabilities
 */
static void packet_check_and_write_capabilities(struct string_list *remote_caps,
						struct string_list *our_caps)
{
	struct string_list_item *item;
	for_each_string_list_item(item, our_caps) {
		if (!unsorted_string_list_has_string(remote_caps, item->string)) {
			die("our capability '%s' is not available from remote",
			    item->string);
		}
		packet_write_fmt(1, "capability=%s\n", item->string);
	}
	packet_flush(1);
}

struct delay_entry {
	int requested, count;
	char *output;
};

static void command_loop(void)
{
	while (1) {
		char *command = packet_key_val_read("command");
		if (!command) {
			fprintf(logfile, "STOP\n");
			break;
		}
		fprintf(logfile, "IN: %s", command);

		if (!strcmp(command, "list_available_blobs")) {
			struct hashmap_iter iter;
			struct strmap_entry *ent;
			struct string_list_item *str_item;
			struct string_list paths = STRING_LIST_INIT_NODUP;

			/* flush */
			if (packet_read_line(0, NULL))
				die("bad list_available_blobs end");

			strmap_for_each_entry(&delay, &iter, ent) {
				struct delay_entry *delay_entry = ent->value;
				if (!delay_entry->requested)
					continue;
				delay_entry->count--;
				if (!strcmp(ent->key, "invalid-delay.a")) {
					/* Send Git a pathname that was not delayed earlier */
					packet_write_fmt(1, "pathname=unfiltered");
				}
				if (!strcmp(ent->key, "missing-delay.a")) {
					/* Do not signal Git that this file is available */
				} else if (!delay_entry->count) {
					string_list_insert(&paths, ent->key);
					packet_write_fmt(1, "pathname=%s", ent->key);
				}
			}

			/* Print paths in sorted order. */
			for_each_string_list_item(str_item, &paths)
				fprintf(logfile, " %s", str_item->string);
			string_list_clear(&paths, 0);

			packet_flush(1);

			fprintf(logfile, " [OK]\n");
			packet_write_fmt(1, "status=success");
			packet_flush(1);
		} else {
			char *buf, *output;
			int size;
			char *pathname;
			struct delay_entry *entry;
			struct strbuf input = STRBUF_INIT;

			pathname = packet_key_val_read("pathname");
			if (!pathname)
				die("unexpected EOF while expecting pathname");
			fprintf(logfile, " %s", pathname);

			/* Read until flush */
			buf = packet_read_line(0, &size);
			while (buf) {
				if (!strcmp(buf, "can-delay=1")) {
					entry = strmap_get(&delay, pathname);
					if (entry && !entry->requested) {
						entry->requested = 1;
					} else if (!entry && always_delay) {
						entry = xcalloc(1, sizeof(*entry));
						entry->requested = 1;
						entry->count = 1;
						strmap_put(&delay, pathname, entry);
					}
				} else if (starts_with(buf, "ref=") ||
					   starts_with(buf, "treeish=") ||
					   starts_with(buf, "blob=")) {
					fprintf(logfile, " %s", buf);
				} else {
					/*
					 * In general, filters need to be graceful about
					 * new metadata, since it's documented that we
					 * can pass any key-value pairs, but for tests,
					 * let's be a little stricter.
					 */
					die("Unknown message '%s'", buf);
				}
				buf = packet_read_line(0, &size);
			}


			read_packetized_to_strbuf(0, &input, 0);
			fprintf(logfile, " %"PRIuMAX" [OK] -- ", (uintmax_t)input.len);

			entry = strmap_get(&delay, pathname);
			if (entry && entry->output) {
				output = entry->output;
			} else if (!strcmp(pathname, "error.r") || !strcmp(pathname, "abort.r")) {
				output = "";
			} else if (!strcmp(command, "clean") && has_capability("clean")) {
				output = rot13(input.buf);
			} else if (!strcmp(command, "smudge") && has_capability("smudge")) {
				output = rot13(input.buf);
			} else {
				die("bad command '%s'", command);
			}

			if (!strcmp(pathname, "error.r")) {
				fprintf(logfile, "[ERROR]\n");
				packet_write_fmt(1, "status=error");
				packet_flush(1);
			} else if (!strcmp(pathname, "abort.r")) {
				fprintf(logfile, "[ABORT]\n");
				packet_write_fmt(1, "status=abort");
				packet_flush(1);
			} else if (!strcmp(command, "smudge") &&
				   (entry = strmap_get(&delay, pathname)) &&
				   entry->requested == 1) {
				fprintf(logfile, "[DELAYED]\n");
				packet_write_fmt(1, "status=delayed");
				packet_flush(1);
				entry->requested = 2;
				entry->output = xstrdup(output);
			} else {
				int i, nr_packets;
				size_t output_len;
				struct strbuf sb = STRBUF_INIT;
				packet_write_fmt(1, "status=success");
				packet_flush(1);

				strbuf_addf(&sb, "%s-write-fail.r", command);
				if (!strcmp(pathname, sb.buf)) {
					fprintf(logfile, "[WRITE FAIL]\n");
					die("%s write error", command);
				}

				output_len = strlen(output);
				fprintf(logfile, "OUT: %"PRIuMAX" ", (uintmax_t)output_len);

				if (write_packetized_from_buf_no_flush_count(output,
					output_len, 1, &nr_packets))
					die("failed to write buffer to stdout");
				packet_flush(1);

				for (i = 0; i < nr_packets; i++)
					fprintf(logfile, ".");
				fprintf(logfile, " [OK]\n");

				packet_flush(1);
				strbuf_release(&sb);
			}
			free(pathname);
			strbuf_release(&input);
		}
		free(command);
	}
}

static void free_delay_hash(void)
{
	struct hashmap_iter iter;
	struct strmap_entry *ent;

	strmap_for_each_entry(&delay, &iter, ent) {
		struct delay_entry *delay_entry = ent->value;
		free(delay_entry->output);
		free(delay_entry);
	}
	strmap_clear(&delay, 0);
}

static void add_delay_entry(char *pathname, int count)
{
	struct delay_entry *entry = xcalloc(1, sizeof(*entry));
	entry->count = count;
	if (strmap_put(&delay, pathname, entry))
		BUG("adding the same path twice to delay hash?");
}

static void packet_initialize(const char *name, int version)
{
	struct strbuf sb = STRBUF_INIT;
	int size;
	char *pkt_buf = packet_read_line(0, &size);

	strbuf_addf(&sb, "%s-client", name);
	if (!pkt_buf || strncmp(pkt_buf, sb.buf, size))
		die("bad initialize: '%s'", xstrndup(pkt_buf, size));

	strbuf_reset(&sb);
	strbuf_addf(&sb, "version=%d", version);
	pkt_buf = packet_read_line(0, &size);
	if (!pkt_buf || strncmp(pkt_buf, sb.buf, size))
		die("bad version: '%s'", xstrndup(pkt_buf, size));

	pkt_buf = packet_read_line(0, &size);
	if (pkt_buf)
		die("bad version end: '%s'", xstrndup(pkt_buf, size));

	packet_write_fmt(1, "%s-server", name);
	packet_write_fmt(1, "version=%d", version);
	packet_flush(1);
	strbuf_release(&sb);
}

static char *rot13_usage = "test-tool rot13-filter [--always-delay] <log path> <capabilities>";

int cmd__rot13_filter(int argc, const char **argv)
{
	int i = 1;
	struct string_list remote_caps = STRING_LIST_INIT_DUP,
			   supported_caps = STRING_LIST_INIT_NODUP;

	string_list_append(&supported_caps, "clean");
	string_list_append(&supported_caps, "smudge");
	string_list_append(&supported_caps, "delay");

	if (argc > 1 && !strcmp(argv[i], "--always-delay")) {
		always_delay = 1;
		i++;
	}
	if (argc - i < 2)
		usage(rot13_usage);

	logfile = fopen(argv[i++], "a");
	if (!logfile)
		die_errno("failed to open log file");

	for ( ; i < argc; i++)
		string_list_append(&requested_caps, argv[i]);

	add_delay_entry("test-delay10.a", 1);
	add_delay_entry("test-delay11.a", 1);
	add_delay_entry("test-delay20.a", 2);
	add_delay_entry("test-delay10.b", 1);
	add_delay_entry("missing-delay.a", 1);
	add_delay_entry("invalid-delay.a", 1);

	fprintf(logfile, "START\n");

	packet_initialize("git-filter", 2);

	packet_read_and_check_capabilities(&remote_caps, &supported_caps);
	packet_check_and_write_capabilities(&remote_caps, &requested_caps);
	fprintf(logfile, "init handshake complete\n");

	string_list_clear(&supported_caps, 0);
	string_list_clear(&remote_caps, 0);

	command_loop();

	fclose(logfile);
	string_list_clear(&requested_caps, 0);
	free_delay_hash();
	return 0;
}
