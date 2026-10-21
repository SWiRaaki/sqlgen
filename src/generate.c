#include "generate.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "builtin.h"
#include "lang.h"
#include "template.h"
#include "value.h"
#include "version.h"

typedef SG_VEC(sg_template_source) source_list;

/* SOURCE_DATE_EPOCH when set, so a build can reproduce its output exactly; else now. */
static bool generation_time(char out[32]) {
	const char *epoch = getenv("SOURCE_DATE_EPOCH");
	time_t      when  = time(NULL);

	if (epoch && *epoch) {
		char      *end   = NULL;
		long long  value = strtoll(epoch, &end, 10);

		if (*end != '\0' || value < 0) {
			sg_error(NULL, 0, 0, "SOURCE_DATE_EPOCH '%s' is not a number of seconds", epoch);
			return false;
		}

		when = (time_t)value;
	}

	struct tm utc;

	if (!gmtime_r(&when, &utc) || strftime(out, 32, "%Y-%m-%d %H:%M:%S UTC", &utc) == 0) {
		sg_error(NULL, 0, 0, "the generation time cannot be written");
		return false;
	}

	return true;
}

static sg_value *template_data(const sg_plan *plan, const sg_target *target, const char *config_arg,
		const char *generated_at) {
	sg_buf json = {0};

	sg_plan_write_json(plan, &json);

	sg_value *root = sg_value_from_json(json.data, json.len);

	sg_buf_free(&json);

	if (!root) {
		sg_error(NULL, 0, 0, "internal: the plan is not readable as data");
		return NULL;
	}

	/* The schema as `sqlgen model` prints it, for templates that describe it rather than wrap it. */
	sg_model_write_json(plan->model, plan->patchset, NULL, &json);

	sg_value *model = sg_value_from_json(json.data, json.len);

	sg_buf_free(&json);

	if (!model) {
		sg_error(NULL, 0, 0, "internal: the model is not readable as data");
		sg_value_free(root);
		return NULL;
	}

	sg_map_set(root, "model", model);

	sg_value *t     = sg_map_set(root, "target", sg_value_map());
	sg_value *types = sg_value_map();

	sg_map_set(t, "language", sg_value_text(target->language));
	sg_map_set(t, "dir", sg_value_text(target->dir));
	sg_map_set(t, "prefix", target->prefix ? sg_value_text(target->prefix) : sg_value_null());

	for (size_t i = 0; i < target->types.count; ++i) {
		sg_map_set(types, target->types.items[i].key, sg_value_text(target->types.items[i].value));
	}

	sg_map_set(t, "types", types);
	sg_map_set(root, "prefix", target->prefix ? sg_value_text(target->prefix) : sg_value_null());

	sg_value *generator = sg_map_set(root, "generator", sg_value_map());

	sg_map_set(generator, "name", sg_value_text(SQLGEN_NAME));
	sg_map_set(generator, "version", sg_value_text(SQLGEN_VERSION));
	sg_map_set(root, "generated_at", sg_value_text(generated_at));
	sg_map_set(root, "command", sg_value_text_owned(sg_format("sqlgen generate -c %s", config_arg)));
	return root;
}

static int compare_names(const void *a, const void *b) {
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Every *.gtpl file of a directory, by name. */
static bool read_templates(const char *dir, source_list *out, sg_str_vec *texts) {
	DIR          *d = opendir(dir);
	sg_str_vec    names = {0};
	struct dirent *entry;
	bool          ok = true;

	if (!d) {
		sg_error(dir, 0, 0, "cannot be read: %s", strerror(errno));
		return false;
	}

	while ((entry = readdir(d)) != NULL) {
		size_t len = strlen(entry->d_name);

		if (len > 5 && strcmp(entry->d_name + len - 5, ".gtpl") == 0) {
			*SG_PUSH(names) = sg_xstrdup(entry->d_name);
		}
	}

	closedir(d);
	qsort(names.items, names.count, sizeof *names.items, compare_names);

	if (names.count == 0) {
		sg_error(dir, 0, 0, "holds no templates (*.gtpl)");
		ok = false;
	}

	for (size_t i = 0; ok && i < names.count; ++i) {
		char   *path = sg_path_join(dir, names.items[i]);
		char   *text = NULL;
		size_t  len  = 0;

		ok = sg_read_file(path, &text, &len);

		if (ok) {
			sg_template_source *source = SG_PUSH(*out);

			source->name = names.items[i];
			source->text = text;
			*SG_PUSH(*texts) = text;
			*SG_PUSH(*texts) = names.items[i];
			names.items[i] = NULL;
		}

		free(path);
	}

	for (size_t i = 0; i < names.count; ++i) {
		free(names.items[i]);
	}

	free(names.items);
	return ok;
}

static bool render_target(const sg_config *config, const sg_plan *plan, size_t index,
		const char *config_arg, const char *generated_at, sg_rendered *out) {
	const sg_target *target = &config->targets.items[index];
	source_list                sources = {0};
	sg_str_vec                 owned   = {0};
	sg_template_set           *set     = NULL;
	sg_value                  *data    = NULL;
	const sg_language         *lang    = sg_language_find(target->language);
	bool                       ok      = lang->check(config, target, index, plan);

	if (ok && target->templates) {
		ok  = read_templates(target->templates, &sources, &owned);
		set = ok ? sg_templates_parse(sources.items, sources.count, target->templates) : NULL;
	} else if (ok) {
		const sg_builtin_set *builtin = sg_builtin_templates(target->language);
		char                 *label   = sg_format("(built-in %s templates)", target->language);

		set = sg_templates_parse(builtin->templates, builtin->count, label);
		free(label);
	}

	ok = ok && set && (data = template_data(plan, target, config_arg, generated_at)) != NULL;

	if (ok) {
		sg_lang_context context = { data, target };
		sg_filter       filters[SG_LANG_MAX_FILTERS];
		size_t          count   = lang->filters(filters, &context);

		ok = sg_templates_render(set, data, filters, count, out);
	}

	sg_value_free(data);
	sg_templates_free(set);

	for (size_t i = 0; i < owned.count; ++i) {
		free(owned.items[i]);
	}

	free(owned.items);
	free(sources.items);
	return ok;
}

/* Entries of a directory other than . and ..; -1 when it does not exist. */
static long entries(const char *dir) {
	DIR           *d = opendir(dir);
	struct dirent *entry;
	long           n = 0;

	if (!d) {
		return -1;
	}

	while ((entry = readdir(d)) != NULL) {
		n += strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0;
	}

	closedir(d);
	return n;
}

static bool confirm(const sg_target *target, long count, bool yes) {
	if (count <= 0 || yes || !target->confirm_clear) {
		return true;
	}

	if (!isatty(STDIN_FILENO)) {
		sg_error(target->dir, 0, 0, "is not empty (%ld %s), and there is no terminal to ask; pass --yes"
				" or set confirm_clear to false", count, count == 1 ? "entry" : "entries");
		return false;
	}

	char answer[16] = "";

	fprintf(stderr, "%s is not empty (%ld %s); clear it and generate? [y/N] ", target->dir, count,
			count == 1 ? "entry" : "entries");
	fflush(stderr);

	if (!fgets(answer, sizeof answer, stdin) || (strcmp(answer, "y\n") != 0 && strcmp(answer, "yes\n") != 0)) {
		sg_error(target->dir, 0, 0, "was left as it is; nothing was generated");
		return false;
	}

	return true;
}

/* Everything inside @p dir, recursively; a symbolic link is removed itself, never followed. */
static bool clear_dir(const char *dir) {
	DIR           *d = opendir(dir);
	struct dirent *entry;
	bool           ok = true;

	if (!d) {
		return errno == ENOENT;
	}

	while (ok && (entry = readdir(d)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		char        *path = sg_path_join(dir, entry->d_name);
		struct stat  info;

		if (lstat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
			ok = clear_dir(path) && rmdir(path) == 0;
		} else {
			ok = unlink(path) == 0;
		}

		if (!ok) {
			sg_error(path, 0, 0, "cannot be removed: %s", strerror(errno));
		}

		free(path);
	}

	closedir(d);
	return ok;
}

/* Every directory along @p path, which names a directory. */
static bool make_dirs(const char *path) {
	char *copy = sg_xstrdup(path);
	bool  ok   = true;

	for (char *slash = copy + 1; ok; ++slash) {
		bool end = *slash == '\0';

		if (*slash != '/' && !end) {
			continue;
		}

		*slash = '\0';

		if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
			sg_error(copy, 0, 0, "cannot be created: %s", strerror(errno));
			ok = false;
		}

		if (end) {
			break;
		}

		*slash = '/';
	}

	free(copy);
	return ok;
}

static bool write_target(const sg_target *target, const sg_rendered *files) {
	bool ok = clear_dir(target->dir) && make_dirs(target->dir);

	for (size_t i = 0; ok && i < files->count; ++i) {
		char *path   = sg_path_join(target->dir, files->items[i].path);
		char *parent = sg_dirname(path);

		ok = make_dirs(parent) && sg_write_file(path, files->items[i].text.data, files->items[i].text.len);
		free(parent);
		free(path);
	}

	if (ok) {
		printf("wrote %zu %s to %s\n", files->count, files->count == 1 ? "file" : "files", target->dir);
	}

	return ok;
}

bool sg_generate(const sg_config *config, const sg_plan *plan, const sg_generate_options *options) {
	SG_VEC(sg_rendered) rendered = {0};
	char                when[32];
	bool                ok = generation_time(when);

	if (ok && config->targets.count == 0) {
		sg_error(config->path, 0, 0, "$.targets is empty, so there is nothing to generate");
		ok = false;
	}

	for (size_t i = 0; ok && i < config->targets.count; ++i) {
		ok = render_target(config, plan, i, options->config_arg, when, SG_PUSH(rendered));
	}

	/* Only now, with everything rendered, is any directory touched. */
	for (size_t i = 0; ok && i < config->targets.count; ++i) {
		ok = confirm(&config->targets.items[i], entries(config->targets.items[i].dir), options->yes);
	}

	for (size_t i = 0; ok && i < config->targets.count; ++i) {
		ok = write_target(&config->targets.items[i], &rendered.items[i]);
	}

	for (size_t i = 0; i < rendered.count; ++i) {
		sg_rendered_free(&rendered.items[i]);
	}

	free(rendered.items);
	return ok;
}

bool sg_export_templates(const char *language, const char *dir) {
	const sg_builtin_set *set = sg_builtin_templates(language);
	bool                  ok  = true;

	if (!set) {
		sg_error(NULL, 0, 0, "no built-in templates for the language '%s'; known: c, cpp, markdown, mermaid", language);
		return false;
	}

	if (!make_dirs(dir)) {
		return false;
	}

	for (size_t i = 0; ok && i < set->count; ++i) {
		char *path = sg_path_join(dir, set->templates[i].name);

		ok = sg_create_file(path, set->templates[i].text, strlen(set->templates[i].text));
		free(path);
	}

	if (ok) {
		printf("wrote %zu %s to %s\n", set->count, set->count == 1 ? "template" : "templates", dir);
	}

	return ok;
}
