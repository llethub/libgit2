/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "refs.h"
#include "hash.h"
#include "repository.h"
#include "fileops.h"
#include "pack.h"
#include "reflog.h"
#include "refdb.h"
#include "refdb_fs.h"
#include "iterator.h"

#include <git2/tag.h>
#include <git2/object.h>
#include <git2/refdb.h>
#include <git2/sys/refdb_backend.h>
#include <git2/sys/refs.h>

GIT__USE_STRMAP;

#define DEFAULT_NESTING_LEVEL	5
#define MAX_NESTING_LEVEL		10

enum {
	PACKREF_HAS_PEEL = 1,
	PACKREF_WAS_LOOSE = 2,
	PACKREF_CANNOT_PEEL = 4,
	PACKREF_SHADOWED = 8,
};

enum {
	PEELING_NONE = 0,
	PEELING_STANDARD,
	PEELING_FULL
};

struct packref {
	git_oid oid;
	git_oid peel;
	char flags;
	char name[GIT_FLEX_ARRAY];
};

typedef struct refdb_fs_backend {
	git_refdb_backend parent;

	git_repository *repo;
	char *path;

	git_refcache refcache;
	int peeling_mode;
} refdb_fs_backend;

static int reference_read(
	git_buf *file_content,
	time_t *mtime,
	const char *repo_path,
	const char *ref_name,
	int *updated)
{
	git_buf path = GIT_BUF_INIT;
	int result;

	assert(file_content && repo_path && ref_name);

	/* Determine the full path of the file */
	if (git_buf_joinpath(&path, repo_path, ref_name) < 0)
		return -1;

	result = git_futils_readbuffer_updated(file_content, path.ptr, mtime, NULL, updated);
	git_buf_free(&path);

	return result;
}

static int packed_parse_oid(
	struct packref **ref_out,
	const char **buffer_out,
	const char *buffer_end)
{
	struct packref *ref = NULL;

	const char *buffer = *buffer_out;
	const char *refname_begin, *refname_end;

	size_t refname_len;
	git_oid id;

	refname_begin = (buffer + GIT_OID_HEXSZ + 1);
	if (refname_begin >= buffer_end || refname_begin[-1] != ' ')
		goto corrupt;

	/* Is this a valid object id? */
	if (git_oid_fromstr(&id, buffer) < 0)
		goto corrupt;

	refname_end = memchr(refname_begin, '\n', buffer_end - refname_begin);
	if (refname_end == NULL)
		refname_end = buffer_end;

	if (refname_end[-1] == '\r')
		refname_end--;

	refname_len = refname_end - refname_begin;

	ref = git__calloc(1, sizeof(struct packref) + refname_len + 1);
	GITERR_CHECK_ALLOC(ref);

	memcpy(ref->name, refname_begin, refname_len);
	ref->name[refname_len] = 0;

	git_oid_cpy(&ref->oid, &id);

	*ref_out = ref;
	*buffer_out = refname_end + 1;
	return 0;

corrupt:
	git__free(ref);
	giterr_set(GITERR_REFERENCE, "The packed references file is corrupted");
	return -1;
}

static int packed_parse_peel(
	struct packref *tag_ref,
	const char **buffer_out,
	const char *buffer_end)
{
	const char *buffer = *buffer_out + 1;

	assert(buffer[-1] == '^');

	/* Ensure it's not the first entry of the file */
	if (tag_ref == NULL)
		goto corrupt;

	if (buffer + GIT_OID_HEXSZ > buffer_end)
		goto corrupt;

	/* Is this a valid object id? */
	if (git_oid_fromstr(&tag_ref->peel, buffer) < 0)
		goto corrupt;

	buffer = buffer + GIT_OID_HEXSZ;
	if (*buffer == '\r')
		buffer++;

	if (buffer != buffer_end) {
		if (*buffer == '\n')
			buffer++;
		else
			goto corrupt;
	}

	tag_ref->flags |= PACKREF_HAS_PEEL;
	*buffer_out = buffer;
	return 0;

corrupt:
	giterr_set(GITERR_REFERENCE, "The packed references file is corrupted");
	return -1;
}

static int packed_load(refdb_fs_backend *backend)
{
	int result, updated;
	git_buf packfile = GIT_BUF_INIT;
	const char *buffer_start, *buffer_end;
	git_refcache *ref_cache = &backend->refcache;

	/* First we make sure we have allocated the hash table */
	if (ref_cache->packfile == NULL) {
		ref_cache->packfile = git_strmap_alloc();
		GITERR_CHECK_ALLOC(ref_cache->packfile);
	}

	if (backend->path == NULL)
		return 0;

	result = reference_read(&packfile, &ref_cache->packfile_time,
		backend->path, GIT_PACKEDREFS_FILE, &updated);

	/*
	 * If we couldn't find the file, we need to clear the table and
	 * return. On any other error, we return that error. If everything
	 * went fine and the file wasn't updated, then there's nothing new
	 * for us here, so just return. Anything else means we need to
	 * refresh the packed refs.
	 */
	if (result == GIT_ENOTFOUND) {
		git_strmap_clear(ref_cache->packfile);
		return 0;
	}

	if (result < 0)
		return -1;

	if (!updated)
		return 0;

	/*
	 * At this point, we want to refresh the packed refs. We already
	 * have the contents in our buffer.
	 */
	git_strmap_clear(ref_cache->packfile);

	buffer_start = (const char *)packfile.ptr;
	buffer_end = (const char *)(buffer_start) + packfile.size;

	backend->peeling_mode = PEELING_NONE;

	if (buffer_start[0] == '#') {
		static const char *traits_header = "# pack-refs with: ";

		if (git__prefixcmp(buffer_start, traits_header) == 0) {
			char *traits = (char *)buffer_start + strlen(traits_header);
			char *traits_end = strchr(traits, '\n');

			if (traits_end == NULL)
				goto parse_failed;

			*traits_end = '\0';

			if (strstr(traits, " fully-peeled ") != NULL) {
				backend->peeling_mode = PEELING_FULL;
			} else if (strstr(traits, " peeled ") != NULL) {
				backend->peeling_mode = PEELING_STANDARD;
			}

			buffer_start = traits_end + 1;
		}
	}

	while (buffer_start < buffer_end && buffer_start[0] == '#') {
		buffer_start = strchr(buffer_start, '\n');
		if (buffer_start == NULL)
			goto parse_failed;

		buffer_start++;
	}

	while (buffer_start < buffer_end) {
		int err;
		struct packref *ref = NULL;

		if (packed_parse_oid(&ref, &buffer_start, buffer_end) < 0)
			goto parse_failed;

		if (buffer_start[0] == '^') {
			if (packed_parse_peel(ref, &buffer_start, buffer_end) < 0)
				goto parse_failed;
		} else if (backend->peeling_mode == PEELING_FULL ||
							(backend->peeling_mode == PEELING_STANDARD &&
							git__prefixcmp(ref->name, GIT_REFS_TAGS_DIR) == 0)) {
			ref->flags |= PACKREF_CANNOT_PEEL;
		}

		git_strmap_insert(ref_cache->packfile, ref->name, ref, err);
		if (err < 0)
			goto parse_failed;
	}

	git_buf_free(&packfile);
	return 0;

parse_failed:
	git_strmap_free(ref_cache->packfile);
	ref_cache->packfile = NULL;
	git_buf_free(&packfile);
	return -1;
}

static int loose_parse_oid(git_oid *oid, const char *filename, git_buf *file_content)
{
	size_t len;
	const char *str;

	len = git_buf_len(file_content);
	if (len < GIT_OID_HEXSZ)
		goto corrupted;

	/* str is guranteed to be zero-terminated */
	str = git_buf_cstr(file_content);

	/* we need to get 40 OID characters from the file */
	if (git_oid_fromstr(oid, git_buf_cstr(file_content)) < 0)
		goto corrupted;

	/* If the file is longer than 40 chars, the 41st must be a space */
	str += GIT_OID_HEXSZ;
	if (*str == '\0' || git__isspace(*str))
		return 0;

corrupted:
	giterr_set(GITERR_REFERENCE, "Corrupted loose reference file: %s", filename);
	return -1;
}

static int loose_lookup_to_packfile(
	struct packref **ref_out,
	refdb_fs_backend *backend,
	const char *name)
{
	git_buf ref_file = GIT_BUF_INIT;
	struct packref *ref = NULL;
	size_t name_len;

	*ref_out = NULL;

	if (reference_read(&ref_file, NULL, backend->path, name, NULL) < 0)
		return -1;

	git_buf_rtrim(&ref_file);

	name_len = strlen(name);
	ref = git__calloc(1, sizeof(struct packref) + name_len + 1);
	GITERR_CHECK_ALLOC(ref);

	memcpy(ref->name, name, name_len);
	ref->name[name_len] = 0;

	if (loose_parse_oid(&ref->oid, name, &ref_file) < 0) {
		git_buf_free(&ref_file);
		git__free(ref);
		return -1;
	}

	ref->flags = PACKREF_WAS_LOOSE;

	*ref_out = ref;
	git_buf_free(&ref_file);
	return 0;
}


static int _dirent_loose_load(void *data, git_buf *full_path)
{
	refdb_fs_backend *backend = (refdb_fs_backend *)data;
	void *old_ref = NULL;
	struct packref *ref;
	const char *file_path;
	int err;

	if (git_path_isdir(full_path->ptr) == true)
		return git_path_direach(full_path, _dirent_loose_load, backend);

	file_path = full_path->ptr + strlen(backend->path);

	if (loose_lookup_to_packfile(&ref, backend, file_path) < 0)
		return -1;

	git_strmap_insert2(
		backend->refcache.packfile, ref->name, ref, old_ref, err);
	if (err < 0) {
		git__free(ref);
		return -1;
	}

	git__free(old_ref);
	return 0;
}

/*
 * Load all the loose references from the repository
 * into the in-memory Packfile, and build a vector with
 * all the references so it can be written back to
 * disk.
 */
static int packed_loadloose(refdb_fs_backend *backend)
{
	git_buf refs_path = GIT_BUF_INIT;
	int result;

	/* the packfile must have been previously loaded! */
	assert(backend->refcache.packfile);

	if (git_buf_joinpath(&refs_path, backend->path, GIT_REFS_DIR) < 0)
		return -1;

	/*
	 * Load all the loose files from disk into the Packfile table.
	 * This will overwrite any old packed entries with their
	 * updated loose versions
	 */
	result = git_path_direach(&refs_path, _dirent_loose_load, backend);
	git_buf_free(&refs_path);

	return result;
}

static int refdb_fs_backend__exists(
	int *exists,
	git_refdb_backend *_backend,
	const char *ref_name)
{
	refdb_fs_backend *backend;
	git_buf ref_path = GIT_BUF_INIT;

	assert(_backend);
	backend = (refdb_fs_backend *)_backend;

	if (packed_load(backend) < 0)
		return -1;

	if (git_buf_joinpath(&ref_path, backend->path, ref_name) < 0)
		return -1;

	if (git_path_isfile(ref_path.ptr) == true ||
		git_strmap_exists(backend->refcache.packfile, ref_path.ptr))
		*exists = 1;
	else
		*exists = 0;

	git_buf_free(&ref_path);
	return 0;
}

static const char *loose_parse_symbolic(git_buf *file_content)
{
	const unsigned int header_len = (unsigned int)strlen(GIT_SYMREF);
	const char *refname_start;

	refname_start = (const char *)file_content->ptr;

	if (git_buf_len(file_content) < header_len + 1) {
		giterr_set(GITERR_REFERENCE, "Corrupted loose reference file");
		return NULL;
	}

	/*
	 * Assume we have already checked for the header
	 * before calling this function
	 */
	refname_start += header_len;

	return refname_start;
}

static int loose_lookup(
	git_reference **out,
	refdb_fs_backend *backend,
	const char *ref_name)
{
	const char *target;
	git_oid oid;
	git_buf ref_file = GIT_BUF_INIT;
	int error = 0;

	error = reference_read(&ref_file, NULL, backend->path, ref_name, NULL);

	if (error < 0)
		goto done;

	if (git__prefixcmp((const char *)(ref_file.ptr), GIT_SYMREF) == 0) {
		git_buf_rtrim(&ref_file);

		if ((target = loose_parse_symbolic(&ref_file)) == NULL) {
			error = -1;
			goto done;
		}

		*out = git_reference__alloc_symbolic(ref_name, target);
	} else {
		if ((error = loose_parse_oid(&oid, ref_name, &ref_file)) < 0)
			goto done;

		*out = git_reference__alloc(ref_name, &oid, NULL);
	}

	if (*out == NULL)
		error = -1;

done:
	git_buf_free(&ref_file);
	return error;
}

static int packed_map_entry(
	struct packref **entry,
	khiter_t *pos,
	refdb_fs_backend *backend,
	const char *ref_name)
{
	git_strmap *packfile_refs;

	if (packed_load(backend) < 0)
		return -1;

	/* Look up on the packfile */
	packfile_refs = backend->refcache.packfile;

	*pos = git_strmap_lookup_index(packfile_refs, ref_name);

	if (!git_strmap_valid_index(packfile_refs, *pos)) {
		giterr_set(GITERR_REFERENCE, "Reference '%s' not found", ref_name);
		return GIT_ENOTFOUND;
	}

	*entry = git_strmap_value_at(packfile_refs, *pos);

	return 0;
}

static int packed_lookup(
	git_reference **out,
	refdb_fs_backend *backend,
	const char *ref_name)
{
	struct packref *entry;
	khiter_t pos;
	int error = 0;

	if ((error = packed_map_entry(&entry, &pos, backend, ref_name)) < 0)
		return error;

	if ((*out = git_reference__alloc(ref_name,
		&entry->oid, &entry->peel)) == NULL)
		return -1;

	return 0;
}

static int refdb_fs_backend__lookup(
	git_reference **out,
	git_refdb_backend *_backend,
	const char *ref_name)
{
	refdb_fs_backend *backend;
	int result;

	assert(_backend);

	backend = (refdb_fs_backend *)_backend;

	if ((result = loose_lookup(out, backend, ref_name)) == 0)
		return 0;

	/* only try to lookup this reference on the packfile if it
	 * wasn't found on the loose refs; not if there was a critical error */
	if (result == GIT_ENOTFOUND) {
		giterr_clear();
		result = packed_lookup(out, backend, ref_name);
	}

	return result;
}

typedef struct {
	git_reference_iterator parent;

	char *glob;
	git_vector loose;
	unsigned int loose_pos;
	khiter_t packed_pos;
} refdb_fs_iter;

static void refdb_fs_backend__iterator_free(git_reference_iterator *_iter)
{
	refdb_fs_iter *iter = (refdb_fs_iter *) _iter;
	char *loose_path;
	size_t i;

	git_vector_foreach(&iter->loose, i, loose_path) {
		git__free(loose_path);
	}

	git_vector_free(&iter->loose);

	git__free(iter->glob);
	git__free(iter);
}

static int iter_load_loose_paths(refdb_fs_backend *backend, refdb_fs_iter *iter)
{
	git_strmap *packfile = backend->refcache.packfile;
	git_buf path = GIT_BUF_INIT;
	git_iterator *fsit;
	const git_index_entry *entry = NULL;

	if (!backend->path) /* do nothing if no path for loose refs */
		return 0;

	if (git_buf_printf(&path, "%s/refs", backend->path) < 0)
		return -1;

	if (git_iterator_for_filesystem(&fsit, git_buf_cstr(&path), 0, NULL, NULL) < 0)
		return -1;

	git_vector_init(&iter->loose, 8, NULL);
	git_buf_sets(&path, GIT_REFS_DIR);

	while (!git_iterator_advance(&entry, fsit)) {
		const char *ref_name;
		khiter_t pos;

		git_buf_truncate(&path, strlen(GIT_REFS_DIR));
		git_buf_puts(&path, entry->path);
		ref_name = git_buf_cstr(&path);

		if (git__suffixcmp(ref_name, ".lock") == 0 ||
			(iter->glob && p_fnmatch(iter->glob, ref_name, 0) != 0))
			continue;

		pos = git_strmap_lookup_index(packfile, ref_name);
		if (git_strmap_valid_index(packfile, pos)) {
			struct packref *ref = git_strmap_value_at(packfile, pos);
			ref->flags |= PACKREF_SHADOWED;
		}

		git_vector_insert(&iter->loose, git__strdup(ref_name));
	}

	git_iterator_free(fsit);
	git_buf_free(&path);

	return 0;
}

static int refdb_fs_backend__iterator_next(
	git_reference **out, git_reference_iterator *_iter)
{
	refdb_fs_iter *iter = (refdb_fs_iter *)_iter;
	refdb_fs_backend *backend = (refdb_fs_backend *)iter->parent.db->backend;
	git_strmap *packfile = backend->refcache.packfile;

	while (iter->loose_pos < iter->loose.length) {
		const char *path = git_vector_get(&iter->loose, iter->loose_pos++);

		if (loose_lookup(out, backend, path) == 0)
			return 0;

		giterr_clear();
	}

	while (iter->packed_pos < kh_end(packfile)) {
		struct packref *ref = NULL;

		while (!kh_exist(packfile, iter->packed_pos)) {
			iter->packed_pos++;
			if (iter->packed_pos == kh_end(packfile))
				return GIT_ITEROVER;
		}

		ref = kh_val(packfile, iter->packed_pos);
		iter->packed_pos++;

		if (ref->flags & PACKREF_SHADOWED)
			continue;

		if (iter->glob && p_fnmatch(iter->glob, ref->name, 0) != 0)
			continue;

		*out = git_reference__alloc(ref->name, &ref->oid, &ref->peel);
		if (*out == NULL)
			return -1;

		return 0;
	}

	return GIT_ITEROVER;
}

static int refdb_fs_backend__iterator_next_name(
	const char **out, git_reference_iterator *_iter)
{
	refdb_fs_iter *iter = (refdb_fs_iter *)_iter;
	refdb_fs_backend *backend = (refdb_fs_backend *)iter->parent.db->backend;
	git_strmap *packfile = backend->refcache.packfile;

	while (iter->loose_pos < iter->loose.length) {
		const char *path = git_vector_get(&iter->loose, iter->loose_pos++);

		if (git_strmap_exists(packfile, path))
			continue;

		*out = path;
		return 0;
	}

	while (iter->packed_pos < kh_end(packfile)) {
		while (!kh_exist(packfile, iter->packed_pos)) {
			iter->packed_pos++;
			if (iter->packed_pos == kh_end(packfile))
				return GIT_ITEROVER;
		}

		*out = kh_key(packfile, iter->packed_pos);
		iter->packed_pos++;

		if (iter->glob && p_fnmatch(iter->glob, *out, 0) != 0)
			continue;

		return 0;
	}

	return GIT_ITEROVER;
}

static int refdb_fs_backend__iterator(
	git_reference_iterator **out, git_refdb_backend *_backend, const char *glob)
{
	refdb_fs_iter *iter;
	refdb_fs_backend *backend;

	assert(_backend);
	backend = (refdb_fs_backend *)_backend;

	if (packed_load(backend) < 0)
		return -1;

	iter = git__calloc(1, sizeof(refdb_fs_iter));
	GITERR_CHECK_ALLOC(iter);

	if (glob != NULL)
		iter->glob = git__strdup(glob);

	iter->parent.next = refdb_fs_backend__iterator_next;
	iter->parent.next_name = refdb_fs_backend__iterator_next_name;
	iter->parent.free = refdb_fs_backend__iterator_free;

	if (iter_load_loose_paths(backend, iter) < 0) {
		refdb_fs_backend__iterator_free((git_reference_iterator *)iter);
		return -1;
	}

	*out = (git_reference_iterator *)iter;
	return 0;
}

static bool ref_is_available(
	const char *old_ref, const char *new_ref, const char *this_ref)
{
	if (old_ref == NULL || strcmp(old_ref, this_ref)) {
		size_t reflen = strlen(this_ref);
		size_t newlen = strlen(new_ref);
		size_t cmplen = reflen < newlen ? reflen : newlen;
		const char *lead = reflen < newlen ? new_ref : this_ref;

		if (!strncmp(new_ref, this_ref, cmplen) && lead[cmplen] == '/') {
			return false;
		}
	}

	return true;
}

static int reference_path_available(
	refdb_fs_backend *backend,
	const char *new_ref,
	const char* old_ref,
	int force)
{
	struct packref *this_ref;

	if (packed_load(backend) < 0)
		return -1;

	if (!force) {
		int exists;

		if (refdb_fs_backend__exists(&exists, (git_refdb_backend *)backend, new_ref) < 0)
			return -1;

		if (exists) {
			giterr_set(GITERR_REFERENCE,
				"Failed to write reference '%s': a reference with "
				" that name already exists.", new_ref);
			return GIT_EEXISTS;
		}
	}

	git_strmap_foreach_value(backend->refcache.packfile, this_ref, {
		if (!ref_is_available(old_ref, new_ref, this_ref->name)) {
			giterr_set(GITERR_REFERENCE,
				"The path to reference '%s' collides with an existing one", new_ref);
			return -1;
		}
	});
	
	return 0;
}

static int loose_write(refdb_fs_backend *backend, const git_reference *ref)
{
	git_filebuf file = GIT_FILEBUF_INIT;
	git_buf ref_path = GIT_BUF_INIT;

	/* Remove a possibly existing empty directory hierarchy
	 * which name would collide with the reference name
	 */
	if (git_futils_rmdir_r(ref->name, backend->path, GIT_RMDIR_SKIP_NONEMPTY) < 0)
		return -1;

	if (git_buf_joinpath(&ref_path, backend->path, ref->name) < 0)
		return -1;

	if (git_filebuf_open(&file, ref_path.ptr, GIT_FILEBUF_FORCE) < 0) {
		git_buf_free(&ref_path);
		return -1;
	}

	git_buf_free(&ref_path);

	if (ref->type == GIT_REF_OID) {
		char oid[GIT_OID_HEXSZ + 1];

		git_oid_fmt(oid, &ref->target.oid);
		oid[GIT_OID_HEXSZ] = '\0';

		git_filebuf_printf(&file, "%s\n", oid);

	} else if (ref->type == GIT_REF_SYMBOLIC) {
		git_filebuf_printf(&file, GIT_SYMREF "%s\n", ref->target.symbolic);
	} else {
		assert(0); /* don't let this happen */
	}

	return git_filebuf_commit(&file, GIT_REFS_FILE_MODE);
}

static int packed_sort(const void *a, const void *b)
{
	const struct packref *ref_a = (const struct packref *)a;
	const struct packref *ref_b = (const struct packref *)b;

	return strcmp(ref_a->name, ref_b->name);
}

/*
 * Find out what object this reference resolves to.
 *
 * For references that point to a 'big' tag (e.g. an
 * actual tag object on the repository), we need to
 * cache on the packfile the OID of the object to
 * which that 'big tag' is pointing to.
 */
static int packed_find_peel(refdb_fs_backend *backend, struct packref *ref)
{
	git_object *object;

	if (ref->flags & PACKREF_HAS_PEEL || ref->flags & PACKREF_CANNOT_PEEL)
		return 0;

	/*
	 * Find the tagged object in the repository
	 */
	if (git_object_lookup(&object, backend->repo, &ref->oid, GIT_OBJ_ANY) < 0)
		return -1;

	/*
	 * If the tagged object is a Tag object, we need to resolve it;
	 * if the ref is actually a 'weak' ref, we don't need to resolve
	 * anything.
	 */
	if (git_object_type(object) == GIT_OBJ_TAG) {
		git_tag *tag = (git_tag *)object;

		/*
		 * Find the object pointed at by this tag
		 */
		git_oid_cpy(&ref->peel, git_tag_target_id(tag));
		ref->flags |= PACKREF_HAS_PEEL;

		/*
		 * The reference has now cached the resolved OID, and is
		 * marked at such. When written to the packfile, it'll be
		 * accompanied by this resolved oid
		 */
	}

	git_object_free(object);
	return 0;
}

/*
 * Write a single reference into a packfile
 */
static int packed_write_ref(struct packref *ref, git_filebuf *file)
{
	char oid[GIT_OID_HEXSZ + 1];

	git_oid_fmt(oid, &ref->oid);
	oid[GIT_OID_HEXSZ] = 0;

	/*
	 * For references that peel to an object in the repo, we must
	 * write the resulting peel on a separate line, e.g.
	 *
	 *	6fa8a902cc1d18527e1355773c86721945475d37 refs/tags/libgit2-0.4
	 *	^2ec0cb7959b0bf965d54f95453f5b4b34e8d3100
	 *
	 * This obviously only applies to tags.
	 * The required peels have already been loaded into `ref->peel_target`.
	 */
	if (ref->flags & PACKREF_HAS_PEEL) {
		char peel[GIT_OID_HEXSZ + 1];
		git_oid_fmt(peel, &ref->peel);
		peel[GIT_OID_HEXSZ] = 0;

		if (git_filebuf_printf(file, "%s %s\n^%s\n", oid, ref->name, peel) < 0)
			return -1;
	} else {
		if (git_filebuf_printf(file, "%s %s\n", oid, ref->name) < 0)
			return -1;
	}

	return 0;
}

/*
 * Remove all loose references
 *
 * Once we have successfully written a packfile,
 * all the loose references that were packed must be
 * removed from disk.
 *
 * This is a dangerous method; make sure the packfile
 * is well-written, because we are destructing references
 * here otherwise.
 */
static int packed_remove_loose(
	refdb_fs_backend *backend,
	git_vector *packing_list)
{
	size_t i;
	git_buf full_path = GIT_BUF_INIT;
	int failed = 0;

	for (i = 0; i < packing_list->length; ++i) {
		struct packref *ref = git_vector_get(packing_list, i);

		if ((ref->flags & PACKREF_WAS_LOOSE) == 0)
			continue;

		if (git_buf_joinpath(&full_path, backend->path, ref->name) < 0)
			return -1; /* critical; do not try to recover on oom */

		if (git_path_exists(full_path.ptr) == true && p_unlink(full_path.ptr) < 0) {
			if (failed)
				continue;

			giterr_set(GITERR_REFERENCE,
				"Failed to remove loose reference '%s' after packing: %s",
				full_path.ptr, strerror(errno));

			failed = 1;
		}

		/*
		 * if we fail to remove a single file, this is *not* good,
		 * but we should keep going and remove as many as possible.
		 * After we've removed as many files as possible, we return
		 * the error code anyway.
		 */
	}

	git_buf_free(&full_path);
	return failed ? -1 : 0;
}

/*
 * Write all the contents in the in-memory packfile to disk.
 */
static int packed_write(refdb_fs_backend *backend)
{
	git_filebuf pack_file = GIT_FILEBUF_INIT;
	size_t i;
	git_buf pack_file_path = GIT_BUF_INIT;
	git_vector packing_list;
	unsigned int total_refs;

	assert(backend && backend->refcache.packfile);

	total_refs =
		(unsigned int)git_strmap_num_entries(backend->refcache.packfile);

	if (git_vector_init(&packing_list, total_refs, packed_sort) < 0)
		return -1;

	/* Load all the packfile into a vector */
	{
		struct packref *reference;

		/* cannot fail: vector already has the right size */
		git_strmap_foreach_value(backend->refcache.packfile, reference, {
			git_vector_insert(&packing_list, reference);
		});
	}

	/* sort the vector so the entries appear sorted on the packfile */
	git_vector_sort(&packing_list);

	/* Now we can open the file! */
	if (git_buf_joinpath(&pack_file_path,
		backend->path, GIT_PACKEDREFS_FILE) < 0)
		goto cleanup_memory;

	if (git_filebuf_open(&pack_file, pack_file_path.ptr, 0) < 0)
		goto cleanup_packfile;

	/* Packfiles have a header... apparently
	 * This is in fact not required, but we might as well print it
	 * just for kicks */
	if (git_filebuf_printf(&pack_file, "%s\n", GIT_PACKEDREFS_HEADER) < 0)
		goto cleanup_packfile;

	for (i = 0; i < packing_list.length; ++i) {
		struct packref *ref = (struct packref *)git_vector_get(&packing_list, i);

		if (packed_find_peel(backend, ref) < 0)
			goto cleanup_packfile;

		if (packed_write_ref(ref, &pack_file) < 0)
			goto cleanup_packfile;
	}

	/* if we've written all the references properly, we can commit
	 * the packfile to make the changes effective */
	if (git_filebuf_commit(&pack_file, GIT_PACKEDREFS_FILE_MODE) < 0)
		goto cleanup_memory;

	/* when and only when the packfile has been properly written,
	 * we can go ahead and remove the loose refs */
	 if (packed_remove_loose(backend, &packing_list) < 0)
		 goto cleanup_memory;

	 {
		struct stat st;
		if (p_stat(pack_file_path.ptr, &st) == 0)
			backend->refcache.packfile_time = st.st_mtime;
	 }

	git_vector_free(&packing_list);
	git_buf_free(&pack_file_path);

	/* we're good now */
	return 0;

cleanup_packfile:
	git_filebuf_cleanup(&pack_file);

cleanup_memory:
	git_vector_free(&packing_list);
	git_buf_free(&pack_file_path);

	return -1;
}

static int refdb_fs_backend__write(
	git_refdb_backend *_backend,
	const git_reference *ref,
	int force)
{
	refdb_fs_backend *backend;
	int error;

	assert(_backend);
	backend = (refdb_fs_backend *)_backend;

	error = reference_path_available(backend, ref->name, NULL, force);
	if (error < 0)
		return error;

	return loose_write(backend, ref);
}

static int refdb_fs_backend__delete(
	git_refdb_backend *_backend,
	const char *ref_name)
{
	refdb_fs_backend *backend;
	git_buf loose_path = GIT_BUF_INIT;
	struct packref *pack_ref;
	khiter_t pack_ref_pos;
	int error = 0;
	bool loose_deleted = 0;

	assert(_backend);
	assert(ref_name);

	backend = (refdb_fs_backend *)_backend;

	/* If a loose reference exists, remove it from the filesystem */
	if (git_buf_joinpath(&loose_path, backend->path, ref_name) < 0)
		return -1;

	if (git_path_isfile(loose_path.ptr)) {
		error = p_unlink(loose_path.ptr);
		loose_deleted = 1;
	}

	git_buf_free(&loose_path);

	if (error != 0)
		return error;

	/* If a packed reference exists, remove it from the packfile and repack */
	error = packed_map_entry(&pack_ref, &pack_ref_pos, backend, ref_name);

	if (error == GIT_ENOTFOUND)
		return loose_deleted ? 0 : GIT_ENOTFOUND;

	if (error == 0) {
		git_strmap_delete_at(backend->refcache.packfile, pack_ref_pos);
		git__free(pack_ref);
		error = packed_write(backend);
	}

	return error;
}

static int refdb_fs_backend__rename(
	git_reference **out,
	git_refdb_backend *_backend,
	const char *old_name,
	const char *new_name,
	int force)
{
	refdb_fs_backend *backend;
	git_reference *old, *new;
	int error;

	assert(_backend);
	backend = (refdb_fs_backend *)_backend;

	error = reference_path_available(backend, new_name, old_name, force);
	if (error < 0)
		return error;

	error = refdb_fs_backend__lookup(&old, _backend, old_name);
	if (error < 0)
		return error;

	error = refdb_fs_backend__delete(_backend, old_name);
	if (error < 0) {
		git_reference_free(old);
		return error;
	}

	new = realloc(old, sizeof(git_reference) + strlen(new_name) + 1);
	memcpy(new->name, new_name, strlen(new_name) + 1);

	error = loose_write(backend, new);
	if (error < 0) {
		git_reference_free(new);
		return error;
	}

	if (out) {
		*out = new;
	} else {
		git_reference_free(new);
	}

	return 0;
}

static int refdb_fs_backend__compress(git_refdb_backend *_backend)
{
	refdb_fs_backend *backend;

	assert(_backend);
	backend = (refdb_fs_backend *)_backend;

	if (packed_load(backend) < 0 || /* load the existing packfile */
		packed_loadloose(backend) < 0 || /* add all the loose refs */
		packed_write(backend) < 0) /* write back to disk */
		return -1;

	return 0;
}

static void refcache_free(git_refcache *refs)
{
	assert(refs);

	if (refs->packfile) {
		struct packref *reference;

		git_strmap_foreach_value(refs->packfile, reference, {
			git__free(reference);
		});

		git_strmap_free(refs->packfile);
	}
}

static void refdb_fs_backend__free(git_refdb_backend *_backend)
{
	refdb_fs_backend *backend;

	assert(_backend);
	backend = (refdb_fs_backend *)_backend;

	refcache_free(&backend->refcache);
	git__free(backend->path);
	git__free(backend);
}

static int setup_namespace(git_buf *path, git_repository *repo)
{
	char *parts, *start, *end;

	/* Not all repositories have a path */
	if (repo->path_repository == NULL)
		return 0;

	/* Load the path to the repo first */
	git_buf_puts(path, repo->path_repository);

	/* if the repo is not namespaced, nothing else to do */
	if (repo->namespace == NULL)
		return 0;

	parts = end = git__strdup(repo->namespace);
	if (parts == NULL)
		return -1;

	/**
	 * From `man gitnamespaces`:
	 *  namespaces which include a / will expand to a hierarchy
	 *  of namespaces; for example, GIT_NAMESPACE=foo/bar will store
	 *  refs under refs/namespaces/foo/refs/namespaces/bar/
	 */
	while ((start = git__strsep(&end, "/")) != NULL) {
		git_buf_printf(path, "refs/namespaces/%s/", start);
	}

	git_buf_printf(path, "refs/namespaces/%s/refs", end);
	git__free(parts);

	/* Make sure that the folder with the namespace exists */
	if (git_futils_mkdir_r(git_buf_cstr(path), repo->path_repository, 0777) < 0)
		return -1;

	/* Return the root of the namespaced path, i.e. without the trailing '/refs' */
	git_buf_rtruncate_at_char(path, '/');
	return 0;
}

int git_refdb_backend_fs(
	git_refdb_backend **backend_out,
	git_repository *repository)
{
	git_buf path = GIT_BUF_INIT;
	refdb_fs_backend *backend;

	backend = git__calloc(1, sizeof(refdb_fs_backend));
	GITERR_CHECK_ALLOC(backend);

	backend->repo = repository;

	if (setup_namespace(&path, repository) < 0) {
		git__free(backend);
		return -1;
	}

	backend->path = git_buf_detach(&path);

	backend->parent.exists = &refdb_fs_backend__exists;
	backend->parent.lookup = &refdb_fs_backend__lookup;
	backend->parent.iterator = &refdb_fs_backend__iterator;
	backend->parent.write = &refdb_fs_backend__write;
	backend->parent.delete = &refdb_fs_backend__delete;
	backend->parent.rename = &refdb_fs_backend__rename;
	backend->parent.compress = &refdb_fs_backend__compress;
	backend->parent.free = &refdb_fs_backend__free;

	*backend_out = (git_refdb_backend *)backend;
	return 0;
}
