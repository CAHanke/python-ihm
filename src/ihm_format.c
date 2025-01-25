/** \file ihm_format.c      Routines for handling mmCIF format files.
 *
 *  The file is read sequentially. All values for desired keywords in
 *  desired categories are collected (other parts of the file are ignored)
 *  At the end of the file a callback function for each category is called
 *  to process the data. In the case of mmCIF loops, this callback will be
 *  called multiple times, one for each entry in the loop.
 */

#include "ihm_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#if defined(_WIN32) || defined(_WIN64)
# include <windows.h>
# include <io.h>
#else
# include <unistd.h>
#endif
#include <errno.h>
#include <assert.h>
#include "cmp.h"

#define INT_TO_POINTER(i) ((void *) (long) (i))
#define POINTER_TO_INT(p) ((int)  (long) (p))

#if defined(_WIN32) || defined(_WIN64)
# define strcasecmp _stricmp
# define usleep Sleep
#endif

/* Allocate memory; unlike malloc() this never returns NULL (a failure will
   terminate the program) */
static void *ihm_malloc(size_t size)
{
  void *ret = malloc(size);
  if (ret) {
    return ret;
  } else {
    fprintf(stderr, "Memory allocation failed\n");
    exit(1);
  }
}

/* Allocate memory; unlike realloc() this never returns NULL (a failure will
   terminate the program) */
static void *ihm_realloc(void *ptr, size_t size)
{
  void *ret = realloc(ptr, size);
  if (ret) {
    return ret;
  } else {
    fprintf(stderr, "Memory allocation failed\n");
    exit(1);
  }
}

/* Free the memory used by an ihm_error */
void ihm_error_free(struct ihm_error *err)
{
  free(err->msg);
  free(err);
}

/* Set the error indicator */
void ihm_error_set(struct ihm_error **err, IHMErrorCode code,
                   const char *format, ...)
{
  va_list ap;
  int len;
  char *msg = NULL;
  assert(err && !*err);

  /* First, determine length needed for complete string */
  va_start(ap, format);
  len = vsnprintf(msg, 0, format, ap);
  va_end(ap);

  msg = (char *)ihm_realloc(msg, len + 1);
  va_start(ap, format);
  vsnprintf(msg, len + 1, format, ap);
  va_end(ap);

  *err = (struct ihm_error *)ihm_malloc(sizeof(struct ihm_error));
  (*err)->code = code;
  (*err)->msg = msg;
}

/* A variable-sized array of elements */
struct ihm_array {
  /* The array data itself */
  void *data;
  /* The number of elements in the array */
  size_t len;
  /* The size in bytes of each element */
  size_t element_size;
  /* The currently-allocated number of elements in the array (>= len) */
  size_t capacity;
};

/* Make a new empty ihm_array */
static struct ihm_array *ihm_array_new(size_t element_size)
{
  struct ihm_array *a = (struct ihm_array *)ihm_malloc(
                                                  sizeof(struct ihm_array));
  a->len = 0;
  a->element_size = element_size;
  a->capacity = 8;
  a->data = ihm_malloc(a->capacity * a->element_size);
  return a;
}

/* Release the memory used by an ihm_array */
static void ihm_array_free(struct ihm_array *a)
{
  free(a->data);
  free(a);
}

/* Set the number of elements in the array to zero */
static void ihm_array_clear(struct ihm_array *a)
{
  a->len = 0;
}

/* Return a reference to the ith element in the array, cast to the given type */
#define ihm_array_index(a, t, i) (((t*)(a)->data)[(i)])

/* Add a new element to the end of the array */
static void ihm_array_append(struct ihm_array *a, void *element)
{
  a->len++;
  if (a->len > a->capacity) {
    a->capacity *= 2;
    a->data = ihm_realloc(a->data, a->capacity * a->element_size);
  }
  memcpy((char *)a->data + (a->len - 1) * a->element_size,
         element, a->element_size);
}

/* A variable-length string buffer */
struct ihm_string {
  /* The string buffer itself */
  char *str;
  /* The length of the string (may be different from strlen(str) if str contains
     embedded nulls); str[len] is always a null byte */
  size_t len;
  /* The allocated size of str; never less than len+1 (to allow for null
     terminator) */
  size_t capacity;
};

/* Make a new ihm_string of zero length */
static struct ihm_string *ihm_string_new(void)
{
  struct ihm_string *s = (struct ihm_string *)ihm_malloc(
                                                sizeof(struct ihm_string));
  s->len = 0;
  s->capacity = 64;
  s->str = (char *)ihm_malloc(s->capacity);
  /* Ensure string is null terminated */
  s->str[0] = '\0';
  return s;
}

/* Free the memory used by an ihm_string */
static void ihm_string_free(struct ihm_string *s)
{
  free(s->str);
  free(s);
}

/* Erase len characters starting at pos from an ihm_string */
static void ihm_string_erase(struct ihm_string *s, size_t pos, size_t len)
{
  memmove(s->str + pos, s->str + pos + len, s->len + 1 - pos - len);
  s->len -= len;
}

/* Set the size of the string to len. If shorter than the current length,
   the string is truncated. If longer, memory (with undefined contents)
   is added to the end of the string */
static void ihm_string_set_size(struct ihm_string *s, size_t len)
{
  if (len >= s->capacity) {
    s->capacity *= 2;
    if (len >= s->capacity) {
      s->capacity = len + 1;
    }
    s->str = (char *)ihm_realloc(s->str, s->capacity);
  }

  s->len = len;
  s->str[s->len] = '\0';
}

/* Set the ihm_string contents to be equal to (null-terminated) str */
static void ihm_string_assign(struct ihm_string *s, const char *str)
{
  size_t len = strlen(str);
  ihm_string_set_size(s, len);
  memcpy(s->str, str, len);
}

/* Set the ihm_string contents to be equal to str of given size */
static void ihm_string_assign_n(struct ihm_string *s, const char *str,
                                size_t strsz)
{
  ihm_string_set_size(s, strsz);
  memcpy(s->str, str, strsz);
}

/* Append str to the end of the ihm_string */
static void ihm_string_append(struct ihm_string *s, const char *str)
{
  size_t len = strlen(str);
  size_t oldlen = s->len;
  ihm_string_set_size(s, s->len + len);
  memcpy(s->str + oldlen, str, len);
}

struct ihm_key_value {
  char *key;
  void *value;
};

/* Function to free mapping values */
typedef void (*ihm_destroy_callback)(void *data);

/* Simple case-insensitive string to struct* mapping using a binary search */
struct ihm_mapping {
  /* Array of struct ihm_key_value */
  struct ihm_array *keyvalues;
  /* Function to free mapping values */
  ihm_destroy_callback value_destroy_func;
};

/* Make a new mapping from case-insensitive strings to arbitrary pointers.
   The mapping uses a simple binary search (more memory efficient than
   a hash table and generally faster too since the number of keys is quite
   small). */
struct ihm_mapping *ihm_mapping_new(ihm_destroy_callback value_destroy_func)
{
  struct ihm_mapping *m = (struct ihm_mapping *)ihm_malloc(
                                                 sizeof(struct ihm_mapping));
  m->keyvalues = ihm_array_new(sizeof(struct ihm_key_value));
  m->value_destroy_func = value_destroy_func;
  return m;
}

/* Clear all key:value pairs from the mapping */
static void ihm_mapping_remove_all(struct ihm_mapping *m)
{
  unsigned i;
  for (i = 0; i < m->keyvalues->len; ++i) {
    (*m->value_destroy_func)(ihm_array_index(m->keyvalues,
                                             struct ihm_key_value, i).value);
  }
  ihm_array_clear(m->keyvalues);
}

/* Free memory used by a mapping */
static void ihm_mapping_free(struct ihm_mapping *m)
{
  ihm_mapping_remove_all(m);
  ihm_array_free(m->keyvalues);
  free(m);
}

/* Add a new key:value pair to the mapping. key is assumed to point to memory
   that is managed elsewhere (and must be valid as long as the mapping exists)
   while value is freed using value_destroy_func when the mapping is freed.
   Neither keys or nor values should ever be NULL. */
static void ihm_mapping_insert(struct ihm_mapping *m, char *key,
                               void *value)
{
  struct ihm_key_value kv;
  kv.key = key;
  kv.value = value;
  ihm_array_append(m->keyvalues, &kv);
}

static int mapping_compare(const void *a, const void *b)
{
  const struct ihm_key_value *kv1, *kv2;
  kv1 = (const struct ihm_key_value *)a;
  kv2 = (const struct ihm_key_value *)b;
  return strcasecmp(kv1->key, kv2->key);
}

/* Put a mapping's key:value pairs in sorted order. This must be done
   before ihm_mapping_lookup is used. */
static void ihm_mapping_sort(struct ihm_mapping *m)
{
  qsort(m->keyvalues->data, m->keyvalues->len, m->keyvalues->element_size,
	mapping_compare);
}

/* Look up key in the mapping and return the corresponding value, or NULL
   if not present. This uses a simple binary search so requires that
   ihm_mapping_sort() has been called first. */
static void *ihm_mapping_lookup(struct ihm_mapping *m, char *key)
{
  int left = 0, right = m->keyvalues->len - 1;

  while (left <= right) {
    int mid = (left + right) / 2;
    int cmp = strcasecmp(ihm_array_index(m->keyvalues, struct ihm_key_value,
                                         mid).key, key);
    if (cmp < 0) {
      left = mid + 1;
    } else if (cmp > 0) {
      right = mid - 1;
    } else {
      return ihm_array_index(m->keyvalues, struct ihm_key_value, mid).value;
    }
  }
  return NULL;
}

/* Callback passed to ihm_mapping_foreach */
typedef void (*ihm_foreach_callback)(void *key, void *value, void *user_data);

/* Call the given function, passing it key, value, and data, for each
   key:value pair in the mapping. */
static void ihm_mapping_foreach(struct ihm_mapping *m,
                                ihm_foreach_callback func, void *data)
{
  unsigned i;
  for (i = 0; i < m->keyvalues->len; ++i) {
    struct ihm_key_value *kv = &ihm_array_index(m->keyvalues,
                                                struct ihm_key_value, i);
    (*func)(kv->key, kv->value, data);
  }
}

/* Free the memory used by a struct ihm_keyword */
static void ihm_keyword_free(void *value)
{
  struct ihm_keyword *key = (struct ihm_keyword *)value;
  free(key->name);
  if (key->own_data && key->in_file) {
    free(key->data);
  }
  free(key);
}

/* A category in an mmCIF file. */
struct ihm_category {
  char *name;
  /* All keywords that we want to extract in this category */
  struct ihm_mapping *keyword_map;
  /* Function called when we have all data for this category */
  ihm_category_callback data_callback;
  /* Function called at the end of each save frame */
  ihm_category_callback end_frame_callback;
  /* Function called at the very end of the data block */
  ihm_category_callback finalize_callback;
  /* Data passed to callbacks */
  void *data;
  /* Function to release data */
  ihm_free_callback free_func;
};

/* Keep track of data used while reading an mmCIF or BinaryCIF file. */
struct ihm_reader {
  /* The file handle to read from */
  struct ihm_file *fh;
  /* true for BinaryCIF, false for mmCIF */
  bool binary;
  /* The current line number in the file */
  int linenum;
  /* Temporary buffer for string data. For mmCIF, this is used for
      multiline tokens, to contain the entire contents of the lines */
  struct ihm_string *tmp_str;
  /* All tokens parsed from the last line */
  struct ihm_array *tokens;
  /* The next token to be returned */
  unsigned token_index;
  /* All categories that we want to extract from the file */
  struct ihm_mapping *category_map;

  /* Handler for unknown categories */
  ihm_unknown_category_callback unknown_category_callback;
  /* Data passed to unknown category callback */
  void *unknown_category_data;
  /* Function to release unknown category data */
  ihm_free_callback unknown_category_free_func;

  /* Handler for unknown keywords */
  ihm_unknown_keyword_callback unknown_keyword_callback;
  /* Data passed to unknown keyword callback */
  void *unknown_keyword_data;
  /* Function to release unknown keyword data */
  ihm_free_callback unknown_keyword_free_func;

  /* msgpack context for reading BinaryCIF file */
  cmp_ctx_t cmp;
  /* Number of BinaryCIF data blocks left to read, or -1 if header
     not read yet */
  int num_blocks_left;
};

typedef enum {
  MMCIF_TOKEN_VALUE = 1,
  MMCIF_TOKEN_OMITTED,
  MMCIF_TOKEN_UNKNOWN,
  MMCIF_TOKEN_LOOP,
  MMCIF_TOKEN_DATA,
  MMCIF_TOKEN_SAVE,
  MMCIF_TOKEN_VARIABLE
} ihm_token_type;

/* Part of a string that corresponds to an mmCIF token. The memory pointed
   to by str is valid only until the next line is read from the file. */
struct ihm_token {
  ihm_token_type type;
  char *str;
};

/* Free memory used by a struct ihm_category */
static void ihm_category_free(void *value)
{
  struct ihm_category *cat = (struct ihm_category *)value;
  ihm_mapping_free(cat->keyword_map);
  free(cat->name);
  if (cat->free_func) {
    (*cat->free_func) (cat->data);
  }
  free(cat);
}

/* Make a new struct ihm_category */
struct ihm_category *ihm_category_new(struct ihm_reader *reader,
                                      const char *name,
                                      ihm_category_callback data_callback,
                                      ihm_category_callback end_frame_callback,
                                      ihm_category_callback finalize_callback,
                                      void *data, ihm_free_callback free_func)
{
  struct ihm_category *category =
        (struct ihm_category *)ihm_malloc(sizeof(struct ihm_category));
  category->name = strdup(name);
  category->data_callback = data_callback;
  category->end_frame_callback = end_frame_callback;
  category->finalize_callback = finalize_callback;
  category->data = data;
  category->free_func = free_func;
  category->keyword_map = ihm_mapping_new(ihm_keyword_free);
  ihm_mapping_insert(reader->category_map, category->name, category);
  return category;
}

/* Add a new struct ihm_keyword to a category. */
struct ihm_keyword *ihm_keyword_new(struct ihm_category *category,
                                    const char *name)
{
  struct ihm_keyword *key =
          (struct ihm_keyword *)ihm_malloc(sizeof(struct ihm_keyword));
  key->name = strdup(name);
  key->own_data = false;
  key->in_file = false;
  ihm_mapping_insert(category->keyword_map, key->name, key);
  key->data = NULL;
  key->own_data = false;
  return key;
}

static void set_keyword_to_default(struct ihm_keyword *key)
{
  key->data = NULL;
  key->own_data = false;
}

/* Set the value of a given keyword from the given string */
static void set_value(struct ihm_reader *reader,
                      struct ihm_category *category,
                      struct ihm_keyword *key, char *str,
                      bool own_data, struct ihm_error **err)
{
  /* If a key is duplicated, overwrite it with the new value */
  if (key->in_file && key->own_data) {
    free(key->data);
  }

  key->omitted = key->unknown = false;

  key->own_data = own_data;
  if (own_data) {
    key->data = strdup(str);
  } else {
    key->data = str;
  }

  key->in_file = true;
}

/* Set the given keyword to the 'omitted' special value */
static void set_omitted_value(struct ihm_keyword *key)
{
  /* If a key is duplicated, overwrite it with the new value */
  if (key->in_file && key->own_data) {
    free(key->data);
  }

  key->omitted = true;
  key->unknown = false;
  set_keyword_to_default(key);
  key->in_file = true;
}

/* Set the given keyword to the 'unknown' special value */
static void set_unknown_value(struct ihm_keyword *key)
{
  /* If a key is duplicated, overwrite it with the new value */
  if (key->in_file && key->own_data) {
    free(key->data);
  }

  key->omitted = false;
  key->unknown = true;
  set_keyword_to_default(key);
  key->in_file = true;
}

/* Make a new ihm_file */
struct ihm_file *ihm_file_new(ihm_file_read_callback read_callback,
                              void *data, ihm_free_callback free_func)
{
  struct ihm_file *file =
           (struct ihm_file *)ihm_malloc(sizeof(struct ihm_file));
  file->buffer = ihm_string_new();
  file->line_start = file->next_line_start = 0;
  file->read_callback = read_callback;
  file->data = data;
  file->free_func = free_func;
  return file;
}

/* Free memory used by ihm_file */
static void ihm_file_free(struct ihm_file *file)
{
  ihm_string_free(file->buffer);
  if (file->free_func) {
    (*file->free_func) (file->data);
  }
  free(file);
}

/* Read data from a file descriptor */
static ssize_t fd_read_callback(char *buffer, size_t buffer_len, void *data,
		                struct ihm_error **err)
{
  int fd = POINTER_TO_INT(data);
  ssize_t readlen;

  while(1) {
#if defined(_WIN32) || defined(_WIN64)
    readlen = _read(fd, buffer, buffer_len);
#else
    readlen = read(fd, buffer, buffer_len);
#endif
    if (readlen != -1 || errno != EAGAIN) break;
    /* If EAGAIN encountered, wait for more data to become available */
    usleep(100);
  }
  if (readlen == -1) {
    ihm_error_set(err, IHM_ERROR_IO, "%s", strerror(errno));
  }
  return readlen;
}

/* Read data from file to expand the in-memory buffer.
   Returns the number of bytes read (0 on EOF), or -1 (and sets err) on error
 */
static ssize_t expand_buffer(struct ihm_file *fh, struct ihm_error **err)
{
  static const size_t READ_SIZE = 4194304; /* Read 4MiB of data at a time */
  size_t current_size;
  ssize_t readlen;

  /* Move any existing data to the start of the buffer (otherwise the buffer
     will grow to the full size of the file) */
  if (fh->line_start) {
    ihm_string_erase(fh->buffer, 0, fh->line_start);
    fh->next_line_start -= fh->line_start;
    fh->line_start = 0;
  }

  current_size = fh->buffer->len;
  ihm_string_set_size(fh->buffer, current_size + READ_SIZE);
  readlen = (*fh->read_callback)(fh->buffer->str + current_size, READ_SIZE,
                                 fh->data, err);
  ihm_string_set_size(fh->buffer, current_size + (readlen == -1 ? 0 : readlen));
  return readlen;
}

/* Read the next line from the file. Lines are terminated by \n, \r, \r\n,
   or \0. On success, true is returned. fh->line_start points to the start of
   the null-terminated line. *eof is set true iff the end of the line is
   the end of the file.
   On error, false is returned and err is set.
 */
static bool ihm_file_read_line(struct ihm_file *fh, int *eof,
                               struct ihm_error **err)
{
  size_t line_end;
  *eof = false;
  fh->line_start = fh->next_line_start;
  if (fh->line_start > fh->buffer->len) {
    /* EOF occurred earlier - return it (plus an empty string) again */
    *eof = true;
    fh->line_start = 0;
    fh->buffer->str[0] = '\0';
    return true;
  }

  /* Line is only definitely terminated if there are characters after it
     (embedded NULL, or \r followed by a possible \n) */
  while((line_end = fh->line_start
           + strcspn(fh->buffer->str + fh->line_start, "\r\n"))
         == fh->buffer->len) {
    ssize_t num_added = expand_buffer(fh, err);
    if (num_added < 0) {
      return false; /* error occurred */
    } else if (num_added == 0) {
      *eof = true; /* end of file */
      break;
    }
  }
  fh->next_line_start = line_end + 1;
  /* Handle \r\n terminator */
  if (fh->buffer->str[line_end] == '\r'
      && fh->buffer->str[line_end + 1] == '\n') {
    fh->next_line_start++;
  }
  fh->buffer->str[line_end] = '\0';
  return true;
}

/* Make a new ihm_file that will read data from the given file descriptor */
struct ihm_file *ihm_file_new_from_fd(int fd)
{
  return ihm_file_new(fd_read_callback, INT_TO_POINTER(fd), NULL);
}

/* Make a new struct ihm_reader */
struct ihm_reader *ihm_reader_new(struct ihm_file *fh, bool binary)
{
  struct ihm_reader *reader =
            (struct ihm_reader *)ihm_malloc(sizeof(struct ihm_reader));
  reader->fh = fh;
  reader->binary = binary;
  reader->linenum = 0;
  reader->tmp_str = ihm_string_new();
  reader->tokens = ihm_array_new(sizeof(struct ihm_token));
  reader->token_index = 0;
  reader->category_map = ihm_mapping_new(ihm_category_free);

  reader->unknown_category_callback = NULL;
  reader->unknown_category_data = NULL;
  reader->unknown_category_free_func = NULL;

  reader->unknown_keyword_callback = NULL;
  reader->unknown_keyword_data = NULL;
  reader->unknown_keyword_free_func = NULL;

  reader->num_blocks_left = -1;
  return reader;
}

/* Free memory used by a struct ihm_reader */
void ihm_reader_free(struct ihm_reader *reader)
{
  ihm_string_free(reader->tmp_str);
  ihm_array_free(reader->tokens);
  ihm_mapping_free(reader->category_map);
  ihm_file_free(reader->fh);
  if (reader->unknown_category_free_func) {
    (*reader->unknown_category_free_func) (reader->unknown_category_data);
  }
  if (reader->unknown_keyword_free_func) {
    (*reader->unknown_keyword_free_func) (reader->unknown_keyword_data);
  }
  free(reader);
}

/* Set a callback for unknown categories.
   The given callback is called whenever a category is encountered in the
   file that is not handled (by ihm_category_new).
 */
void ihm_reader_unknown_category_callback_set(struct ihm_reader *reader,
                                     ihm_unknown_category_callback callback,
                                     void *data, ihm_free_callback free_func)
{
  if (reader->unknown_category_free_func) {
    (*reader->unknown_category_free_func) (reader->unknown_category_data);
  }
  reader->unknown_category_callback = callback;
  reader->unknown_category_data = data;
  reader->unknown_category_free_func = free_func;
}

/* Set a callback for unknown keywords.
   The given callback is called whenever a keyword is encountered in the
   file that is not handled (within a category that is handled by
   ihm_category_new).
 */
void ihm_reader_unknown_keyword_callback_set(struct ihm_reader *reader,
                                     ihm_unknown_keyword_callback callback,
                                     void *data, ihm_free_callback free_func)
{
  if (reader->unknown_keyword_free_func) {
    (*reader->unknown_keyword_free_func) (reader->unknown_keyword_data);
  }
  reader->unknown_keyword_callback = callback;
  reader->unknown_keyword_data = data;
  reader->unknown_keyword_free_func = free_func;
}

/* Remove all categories from the reader. */
void ihm_reader_remove_all_categories(struct ihm_reader *reader)
{
  ihm_mapping_remove_all(reader->category_map);
  if (reader->unknown_category_free_func) {
    (*reader->unknown_category_free_func) (reader->unknown_category_data);
  }
  reader->unknown_category_callback = NULL;
  reader->unknown_category_data = NULL;
  reader->unknown_category_free_func = NULL;

  if (reader->unknown_keyword_free_func) {
    (*reader->unknown_keyword_free_func) (reader->unknown_keyword_data);
  }
  reader->unknown_keyword_callback = NULL;
  reader->unknown_keyword_data = NULL;
  reader->unknown_keyword_free_func = NULL;
}

/* Given the start of a quoted string, find the end and add a token for it */
static size_t handle_quoted_token(struct ihm_reader *reader,
                                  char *line, size_t len,
                                  size_t start_pos, const char *quote_type,
                                  struct ihm_error **err)
{
  char *pt = line + start_pos;
  char *end = pt;
  /* Get the next quote that is followed by whitespace (or line end).
     In mmCIF a quote within a string is not considered an end quote as
     long as it is not followed by whitespace. */
  do {
    end = strchr(end + 1, pt[0]);
  } while (end && *end && end[1] && !strchr(" \t", end[1]));
  if (end && *end) {
    struct ihm_token t;
    int tok_end = end - pt + start_pos;
    /* A quoted string is always a literal string, even if it is
       "?" or ".", not an unknown/omitted value */
    t.type = MMCIF_TOKEN_VALUE;
    t.str = line + start_pos + 1;
    line[tok_end] = '\0';
    ihm_array_append(reader->tokens, &t);
    return tok_end + 1;         /* step past the closing quote */
  } else {
    ihm_error_set(err, IHM_ERROR_FILE_FORMAT,
                  "%s-quoted string not terminated in file, line %d",
                  quote_type, reader->linenum);
    return len;
  }
}

/* Get the next token from the line. */
static size_t get_next_token(struct ihm_reader *reader, char *line,
                             size_t len, size_t start_pos,
                             struct ihm_error **err)
{
  /* Skip initial whitespace */
  char *pt = line + start_pos;
  start_pos += strspn(pt, " \t");
  pt = line + start_pos;
  if (*pt == '\0') {
    return len;
  } else if (*pt == '"') {
    return handle_quoted_token(reader, line, len, start_pos, "Double", err);
  } else if (*pt == '\'') {
    return handle_quoted_token(reader, line, len, start_pos, "Single", err);
  } else if (*pt == '#') {
    /* Comment - discard the rest of the line */
    return len;
  } else {
    struct ihm_token t;
    int tok_end = start_pos + strcspn(pt, " \t");
    t.str = line + start_pos;
    line[tok_end] = '\0';
    if (strcmp(t.str, "loop_") == 0) {
      t.type = MMCIF_TOKEN_LOOP;
    } else if (strncmp(t.str, "data_", 5) == 0) {
      t.type = MMCIF_TOKEN_DATA;
    } else if (strncmp(t.str, "save_", 5) == 0) {
      t.type = MMCIF_TOKEN_SAVE;
    } else if (t.str[0] == '_') {
      t.type = MMCIF_TOKEN_VARIABLE;
    } else if (t.str[0] == '.' && t.str[1] == '\0') {
      t.type = MMCIF_TOKEN_OMITTED;
    } else if (t.str[0] == '?' && t.str[1] == '\0') {
      t.type = MMCIF_TOKEN_UNKNOWN;
    } else {
      /* Note that we do no special processing for other reserved words
         (global_, stop_). But the probability of them occurring
         where we expect a value is pretty small. */
      t.type = MMCIF_TOKEN_VALUE;
    }
    ihm_array_append(reader->tokens, &t);
    return tok_end + 1;
  }
}

/* Break up a line into tokens, populating reader->tokens. */
static void tokenize(struct ihm_reader *reader, char *line,
                     struct ihm_error **err)
{
  size_t start_pos, len = strlen(line);
  ihm_array_clear(reader->tokens);
  if (len > 0 && line[0] == '#') {
    /* Skip comment lines */
    return;
  }
  for (start_pos = 0; start_pos < len && !*err;
       start_pos = get_next_token(reader, line, len, start_pos, err)) {
  }
  if (*err) {
    ihm_array_clear(reader->tokens);
  }
}

/* Return a pointer to the current line */
static char *line_pt(struct ihm_reader *reader)
{
  return reader->fh->buffer->str + reader->fh->line_start;
}

/* Read a semicolon-delimited (multiline) token */
static void read_multiline_token(struct ihm_reader *reader,
                                 int ignore_multiline, struct ihm_error **err)
{
  int eof = 0;
  int start_linenum = reader->linenum;
  while (!eof) {
    reader->linenum++;
    if (!ihm_file_read_line(reader->fh, &eof, err)) {
      return;
    } else if (line_pt(reader)[0] == ';') {
      struct ihm_token t;
      t.type = MMCIF_TOKEN_VALUE;
      t.str = reader->tmp_str->str;
      ihm_array_clear(reader->tokens);
      ihm_array_append(reader->tokens, &t);
      reader->token_index = 0;
      return;
    } else if (!ignore_multiline) {
      ihm_string_append(reader->tmp_str, "\n");
      ihm_string_append(reader->tmp_str, line_pt(reader));
    }
  }
  ihm_error_set(err, IHM_ERROR_FILE_FORMAT,
                "End of file while reading multiline string "
                "which started on line %d", start_linenum);
}

/* Return the number of tokens still available in the current line. */
static unsigned get_num_line_tokens(struct ihm_reader *reader)
{
  return reader->tokens->len - reader->token_index;
}

/* Push back the last token returned by get_token() so it can
   be read again. */
static void unget_token(struct ihm_reader *reader)
{
  reader->token_index--;
}

/* Get the next token from an mmCIF file, or NULL on end of file.
   The memory used by the token is valid for N calls to this function, where
   N is the result of get_num_line_tokens().
   If ignore_multiline is true, the string contents of any multiline
   value tokens (those that are semicolon-delimited) are not stored
   in memory. */
static struct ihm_token *get_token(struct ihm_reader *reader,
                                   int ignore_multiline,
                                   struct ihm_error **err)
{
  int eof = 0;
  if (reader->tokens->len <= reader->token_index) {
    do {
      /* No tokens left - read the next non-blank line in */
      reader->linenum++;
      if (!ihm_file_read_line(reader->fh, &eof, err)) {
        return NULL;
      } else if (line_pt(reader)[0] == ';') {
        if (!ignore_multiline) {
          /* Skip initial semicolon */
          ihm_string_assign(reader->tmp_str, line_pt(reader) + 1);
        }
        read_multiline_token(reader, ignore_multiline, err);
        if (*err) {
          return NULL;
        }
      } else {
        tokenize(reader, line_pt(reader), err);
        if (*err) {
          return NULL;
        } else {
          reader->token_index = 0;
        }
      }
    } while (reader->tokens->len == 0 && !eof);
  }
  if (reader->tokens->len == 0) {
    return NULL;
  } else {
    return &ihm_array_index(reader->tokens, struct ihm_token,
                            reader->token_index++);
  }
}

/* Break up a variable token into category and keyword */
static void parse_category_keyword(struct ihm_reader *reader,
                                   char *str, char **category,
                                   char **keyword, struct ihm_error **err)
{
  char *dot;
  size_t wordlen;
  dot = strchr(str, '.');
  if (!dot) {
    ihm_error_set(err, IHM_ERROR_FILE_FORMAT,
                  "No period found in mmCIF variable name (%s) at line %d",
                  str, reader->linenum);
    return;
  }
  wordlen = strcspn(str, " \t");
  str[wordlen] = '\0';
  *dot = '\0';
  *category = str;
  *keyword = dot + 1;
}

/* Read a line that sets a single value, e.g. _entry.id   1YTI */
static void read_value(struct ihm_reader *reader,
                       struct ihm_token *key_token, struct ihm_error **err)
{
  struct ihm_category *category;
  char *category_name, *keyword_name;
  parse_category_keyword(reader, key_token->str, &category_name,
                         &keyword_name, err);
  if (*err)
    return;

  category = (struct ihm_category *)ihm_mapping_lookup(reader->category_map,
                                                       category_name);
  if (category) {
    struct ihm_keyword *key;
    key = (struct ihm_keyword *)ihm_mapping_lookup(category->keyword_map,
                                                   keyword_name);
    if (key) {
      struct ihm_token *val_token = get_token(reader, false, err);
      if (val_token && val_token->type == MMCIF_TOKEN_VALUE) {
        set_value(reader, category, key, val_token->str, true, err);
      } else if (val_token && val_token->type == MMCIF_TOKEN_OMITTED) {
        set_omitted_value(key);
      } else if (val_token && val_token->type == MMCIF_TOKEN_UNKNOWN) {
        set_unknown_value(key);
      } else if (!*err) {
        ihm_error_set(err, IHM_ERROR_FILE_FORMAT,
                      "No valid value found for %s.%s in file, line %d",
                      category->name, key->name, reader->linenum);
      }
    } else if (reader->unknown_keyword_callback) {
      (*reader->unknown_keyword_callback)(reader, category_name, keyword_name,
                                          reader->linenum,
                                          reader->unknown_keyword_data, err);
    }
  } else if (reader->unknown_category_callback) {
    (*reader->unknown_category_callback)(reader, category_name,
                                         reader->linenum,
                                         reader->unknown_category_data, err);
  }
}

/* Handle a single token listing category and keyword from a loop_ construct.
   The relevant ihm_keyword is returned, or NULL if we are not interested
   in this keyword. */
static struct ihm_keyword *handle_loop_index(struct ihm_reader *reader,
                                             struct ihm_category **catpt,
                                             struct ihm_token *token,
                                             bool first_loop,
                                             struct ihm_error **err)
{
  struct ihm_category *category;
  char *category_name, *keyword_name;
  parse_category_keyword(reader, token->str, &category_name,
                         &keyword_name, err);
  if (*err)
    return NULL;

  category = (struct ihm_category *)ihm_mapping_lookup(reader->category_map,
                                                       category_name);
  if (first_loop) {
    *catpt = category;
    if (!category && reader->unknown_category_callback) {
      (*reader->unknown_category_callback)(reader, category_name,
                                           reader->linenum,
                                           reader->unknown_category_data, err);
      if (*err) {
        return NULL;
      }
    }
  } else if (*catpt != category) {
    ihm_error_set(err, IHM_ERROR_FILE_FORMAT,
                  "mmCIF files cannot contain multiple categories "
                  "within a single loop at line %d", reader->linenum);
    return NULL;
  }
  if (category) {
    struct ihm_keyword *key;
    key = (struct ihm_keyword *)ihm_mapping_lookup(category->keyword_map,
                                                   keyword_name);
    if (key) {
      return key;
    } else if (reader->unknown_keyword_callback) {
      (*reader->unknown_keyword_callback)(reader, category_name, keyword_name,
                                          reader->linenum,
                                          reader->unknown_keyword_data, err);
      if (*err) {
        return NULL;
      }
    }
  }
  return NULL;
}

static void check_keywords_in_file(void *k, void *value, void *user_data)
{
  struct ihm_keyword *key = (struct ihm_keyword *)value;
  bool *in_file = (bool *)user_data;
  *in_file |= key->in_file;
}

static void clear_keywords(void *k, void *value, void *user_data)
{
  struct ihm_keyword *key = (struct ihm_keyword *)value;
  if (key->own_data) {
    free(key->data);
  }
  key->in_file = false;
  set_keyword_to_default(key);
}

/* Call the category's data callback function.
   If force is false, only call it if data has actually been read in. */
static void call_category(struct ihm_reader *reader,
                          struct ihm_category *category, bool force,
                          struct ihm_error **err)
{
  if (category->data_callback) {
    if (!force) {
      /* Check to see if at least one keyword was given a value */
      ihm_mapping_foreach(category->keyword_map, check_keywords_in_file,
                          &force);
    }
    if (force) {
      (*category->data_callback) (reader, category->data, err);
    }
  }
  /* Clear out keyword values, ready for the next set of data */
  ihm_mapping_foreach(category->keyword_map, clear_keywords, NULL);
}

/* Read the list of keywords from a loop_ construct. */
static struct ihm_array *read_loop_keywords(struct ihm_reader *reader,
                                            struct ihm_category **category,
                                            struct ihm_error **err)
{
  bool first_loop = true;
  struct ihm_token *token;
  /* An array of ihm_keyword*, in the order the values should be given.
     Any NULL pointers correspond to keywords we're not interested in. */
  struct ihm_array *keywords = ihm_array_new(sizeof(struct ihm_keyword*));
  *category = NULL;

  while (!*err && (token = get_token(reader, false, err))) {
    if (token->type == MMCIF_TOKEN_VARIABLE) {
      struct ihm_keyword *k = handle_loop_index(reader, category,
                                                token, first_loop, err);
      ihm_array_append(keywords, &k);
      first_loop = false;
    } else if (token->type == MMCIF_TOKEN_VALUE
               || token->type == MMCIF_TOKEN_UNKNOWN
               || token->type == MMCIF_TOKEN_OMITTED) {
      /* OK, end of keywords; proceed on to values */
      unget_token(reader);
      break;
    } else {
      ihm_error_set(err, IHM_ERROR_FILE_FORMAT,
                    "Was expecting a keyword or value for loop at line %d",
                    reader->linenum);
    }
  }
  if (*err) {
    ihm_array_free(keywords);
    return NULL;
  } else {
    return keywords;
  }
}

/* Read data for a loop_ construct */
static void read_loop_data(struct ihm_reader *reader,
                           struct ihm_category *category, unsigned len,
                           struct ihm_keyword **keywords,
                           struct ihm_error **err)
{
  while (!*err) {
    /* Does the current line contain an entire row in the loop? */
    int oneline = get_num_line_tokens(reader) >= len;
    unsigned i;
    for (i = 0; !*err && i < len; ++i) {
      struct ihm_token *token = get_token(reader, false, err);
      if (*err) {
        break;
      } else if (token && token->type == MMCIF_TOKEN_VALUE) {
        if (keywords[i]) {
          set_value(reader, category, keywords[i], token->str, !oneline, err);
        }
      } else if (token && token->type == MMCIF_TOKEN_OMITTED) {
        if (keywords[i]) {
          set_omitted_value(keywords[i]);
        }
      } else if (token && token->type == MMCIF_TOKEN_UNKNOWN) {
        if (keywords[i]) {
          set_unknown_value(keywords[i]);
        }
      } else if (i == 0) {
        /* OK, end of the loop */
        if (token) {
          unget_token(reader);
        }
        return;
      } else {
        ihm_error_set(err, IHM_ERROR_FILE_FORMAT,
                      "Wrong number of data values in loop (should be an "
                      "exact multiple of the number of keys) at line %d",
                      reader->linenum);
      }
    }
    if (!*err) {
      call_category(reader, category, true, err);
    }
  }
}

/* Read a loop_ construct from the file. */
static void read_loop(struct ihm_reader *reader, struct ihm_error **err)
{
  struct ihm_array *keywords;
  struct ihm_category *category;

  keywords = read_loop_keywords(reader, &category, err);
  if (*err) {
    return;
  }
  if (category) {
    read_loop_data(reader, category, keywords->len,
                   (struct ihm_keyword **)keywords->data, err);
  }
  ihm_array_free(keywords);
}

struct category_foreach_data {
  struct ihm_error **err;
  struct ihm_reader *reader;
};

static void call_category_foreach(void *key, void *value, void *user_data)
{
  struct category_foreach_data *d = (struct category_foreach_data *)user_data;
  struct ihm_category *category = (struct ihm_category *)value;
  if (!*(d->err)) {
    call_category(d->reader, category, false, d->err);
  }
}

/* Process any data stored in all categories */
static void call_all_categories(struct ihm_reader *reader,
                                struct ihm_error **err)
{
  struct category_foreach_data d;
  d.err = err;
  d.reader = reader;
  ihm_mapping_foreach(reader->category_map, call_category_foreach, &d);
}

static void finalize_category_foreach(void *key, void *value, void *user_data)
{
  struct category_foreach_data *d = (struct category_foreach_data *)user_data;
  struct ihm_category *category = (struct ihm_category *)value;
  if (!*(d->err) && category->finalize_callback) {
    (*category->finalize_callback)(d->reader, category->data, d->err);
  }
}

/* Call each category's finalize callback */
static void finalize_all_categories(struct ihm_reader *reader,
                                    struct ihm_error **err)
{
  struct category_foreach_data d;
  d.err = err;
  d.reader = reader;
  ihm_mapping_foreach(reader->category_map, finalize_category_foreach, &d);
}

static void end_frame_category_foreach(void *key, void *value, void *user_data)
{
  struct category_foreach_data *d = (struct category_foreach_data *)user_data;
  struct ihm_category *category = (struct ihm_category *)value;
  if (!*(d->err) && category->end_frame_callback) {
    (*category->end_frame_callback)(d->reader, category->data, d->err);
  }
}

/* Call each category's end_frame callback */
static void end_frame_all_categories(struct ihm_reader *reader,
                                     struct ihm_error **err)
{
  struct category_foreach_data d;
  d.err = err;
  d.reader = reader;
  ihm_mapping_foreach(reader->category_map, end_frame_category_foreach, &d);
}

static void sort_category_foreach(void *key, void *value, void *user_data)
{
  struct ihm_category *category = (struct ihm_category *)value;
  ihm_mapping_sort(category->keyword_map);
}

/* Make sure that all mappings are sorted before we try to use them */
static void sort_mappings(struct ihm_reader *reader)
{
  ihm_mapping_sort(reader->category_map);
  ihm_mapping_foreach(reader->category_map, sort_category_foreach, NULL);
}

/* Read an entire mmCIF file. */
static bool read_mmcif_file(struct ihm_reader *reader, bool *more_data,
                            struct ihm_error **err)
{
  int ndata = 0, in_save = 0;
  struct ihm_token *token;
  sort_mappings(reader);
  while (!*err && (token = get_token(reader, true, err))) {
    if (token->type == MMCIF_TOKEN_VARIABLE) {
      read_value(reader, token, err);
    } else if (token->type == MMCIF_TOKEN_DATA) {
      ndata++;
      /* Only read the first data block */
      if (ndata > 1) {
        /* Allow reading the next data block */
        unget_token(reader);
        break;
      }
    } else if (token->type == MMCIF_TOKEN_LOOP) {
      read_loop(reader, err);
    } else if (token->type == MMCIF_TOKEN_SAVE) {
      in_save = !in_save;
      if (!in_save) {
        call_all_categories(reader, err);
        end_frame_all_categories(reader, err);
      }
    }
  }
  if (!*err) {
    call_all_categories(reader, err);
    finalize_all_categories(reader, err);
  }
  if (*err) {
    return false;
  } else {
    *more_data = (ndata > 1);
    return true;
  }
}

/* Read exactly sz bytes from the given file. Return a pointer to the
   location in the file read buffer of those bytes. This pointer is only
   valid until the next file read. */
static bool ihm_file_read_bytes(struct ihm_file *fh, char **buf, size_t sz,
                                struct ihm_error **err)
{
  /* Read at least 4MiB of data at a time */
  static const ssize_t READ_SIZE = 4194304;
  if (fh->line_start + sz > fh->buffer->len) {
    size_t current_size, to_read;
    ssize_t readlen, needed;
    /* Move any existing data to the start of the buffer, so it doesn't
       grow to the full size of the file */
    if (fh->line_start) {
      ihm_string_erase(fh->buffer, 0, fh->line_start);
      fh->line_start = 0;
    }
    /* Fill buffer with new data, at least sz long (but could be more) */
    current_size = fh->buffer->len;
    needed = sz - current_size;
    to_read = READ_SIZE > needed ? READ_SIZE : needed;
    /* Expand buffer as needed */
    ihm_string_set_size(fh->buffer, current_size + to_read);
    readlen = (*fh->read_callback)(
          fh->buffer->str + current_size, to_read, fh->data, err);
    if (readlen < needed) {
      ihm_error_set(err, IHM_ERROR_IO, "Less data read than requested");
      return false;
    }
    /* Set buffer size to match data actually read */
    ihm_string_set_size(fh->buffer, current_size + readlen);
  }
  *buf = fh->buffer->str + fh->line_start;
  fh->line_start += sz;
  return true;
}

/* Read callback for the cmp library */
static bool bcif_cmp_read(cmp_ctx_t *ctx, void *data, size_t limit)
{
  char *buf;
  struct ihm_error *err = NULL;
  struct ihm_reader *reader = (struct ihm_reader *)ctx->buf;
  if (!ihm_file_read_bytes(reader->fh, &buf, limit, &err)) {
    ihm_error_free(err); /* todo: pass IO error back */
    return false;
  } else {
    memcpy(data, buf, limit);
    return true;
  }
}

/* Skip callback for the cmp library */
static bool bcif_cmp_skip(cmp_ctx_t *ctx, size_t count)
{
  char *buf;
  struct ihm_error *err = NULL;
  struct ihm_reader *reader = (struct ihm_reader *)ctx->buf;
  if (!ihm_file_read_bytes(reader->fh, &buf, count, &err)) {
    ihm_error_free(err); /* todo: pass IO error back */
    return false;
  } else {
    return true;
  }
}

/* Read the next msgpack object from the BinaryCIF file; it must be a map.
   Return true on success and return the number of elements in the map;
   return false on error (and set err)
 */
static bool read_bcif_map(struct ihm_reader *reader, uint32_t *map_size,
                          struct ihm_error **err)
{
  if (!cmp_read_map(&reader->cmp, map_size)) {
    ihm_error_set(err, IHM_ERROR_FILE_FORMAT, "Was expecting a map; %s",
                  cmp_strerror(&reader->cmp));
    return false;
  } else {
    return true;
  }
}

/* Read the next msgpack object from the BinaryCIF file; it must be an array.
   Return true on success and return the number of elements in the array;
   return false on error (and set err)
 */
static bool read_bcif_array(struct ihm_reader *reader, uint32_t *array_size,
                            struct ihm_error **err)
{
  if (!cmp_read_array(&reader->cmp, array_size)) {
    ihm_error_set(err, IHM_ERROR_FILE_FORMAT, "Was expecting an array; %s",
                  cmp_strerror(&reader->cmp));
    return false;
  } else {
    return true;
  }
}

/* Skip the next msgpack object from the BinaryCIF file; it can be any kind
   of simple object (not an array or map).
 */
static bool skip_bcif_object(struct ihm_reader *reader, struct ihm_error **err)
{
  if (!cmp_skip_object(&reader->cmp, NULL)) {
    ihm_error_set(err, IHM_ERROR_FILE_FORMAT, "Could not skip object; %s",
                  cmp_strerror(&reader->cmp));
    return false;
  } else {
    return true;
  }
}

/* Skip the next msgpack object from the BinaryCIF file; it can be any kind
   of object, including an array or map.
 */
static bool skip_bcif_object_no_limit(struct ihm_reader *reader,
                                      struct ihm_error **err)
{
  if (!cmp_skip_object_no_limit(&reader->cmp)) {
    ihm_error_set(err, IHM_ERROR_FILE_FORMAT, "Could not skip object; %s",
                  cmp_strerror(&reader->cmp));
    return false;
  } else {
    return true;
  }
}

/* Read the next integer object from the BinaryCIF file.
 */
static bool read_bcif_int(struct ihm_reader *reader, int32_t *value,
                          struct ihm_error **err)
{
  if (!cmp_read_int(&reader->cmp, value)) {
    ihm_error_set(err, IHM_ERROR_FILE_FORMAT, "Was expecting an integer; %s",
                  cmp_strerror(&reader->cmp));
    return false;
  } else {
    return true;
  }
}

/* Read the next bool object from the BinaryCIF file.
 */
static bool read_bcif_bool(struct ihm_reader *reader, bool *value,
                          struct ihm_error **err)
{
  if (!cmp_read_bool(&reader->cmp, value)) {
    ihm_error_set(err, IHM_ERROR_FILE_FORMAT, "Was expecting a boolean; %s",
                  cmp_strerror(&reader->cmp));
    return false;
  } else {
    return true;
  }
}

/* Read the next string from the BinaryCIF file and return a pointer to it.
   This pointer points into ihm_reader and is valid until the next read. */
static bool read_bcif_string(struct ihm_reader *reader, char **str,
                             struct ihm_error **err)
{
  char *buf;
  uint32_t strsz;
  if (!cmp_read_str_size(&reader->cmp, &strsz)) {
    ihm_error_set(err, IHM_ERROR_FILE_FORMAT, "Was expecting a string; %s",
                  cmp_strerror(&reader->cmp));
    return false;
  }
  if (!ihm_file_read_bytes(reader->fh, &buf, strsz, err)) return false;
  /* Copy into reader's temporary string buffer and return a pointer to it */
  ihm_string_assign_n(reader->tmp_str, buf, strsz);
  *str = reader->tmp_str->str;
  return true;
}

/* Read the next string from the BinaryCIF file and store a copy of it at
   the given pointer. The caller is responsible for freeing it later. */
static bool read_bcif_string_dup(struct ihm_reader *reader, char **str,
                                 struct ihm_error **err)
{
  char *buf;
  uint32_t strsz;
  if (!cmp_read_str_size(&reader->cmp, &strsz)) {
    ihm_error_set(err, IHM_ERROR_FILE_FORMAT, "Was expecting a string; %s",
                  cmp_strerror(&reader->cmp));
    return false;
  }
  if (!ihm_file_read_bytes(reader->fh, &buf, strsz, err)) return false;
  /* strdup into new buffer */
  free(*str);
  *str = strndup(buf, strsz);
  return true;
}

/* Read the next string from the BinaryCIF file. Set match if it compares
   equal to str. This is slightly more efficient than returning the
   null-terminated string and then comparing it as it eliminates a copy. */
static bool read_bcif_exact_string(struct ihm_reader *reader, const char *str,
                                   bool *match, struct ihm_error **err)
{
  char *buf;
  uint32_t actual_len, want_len = strlen(str);
  if (!cmp_read_str_size(&reader->cmp, &actual_len)) {
    ihm_error_set(err, IHM_ERROR_FILE_FORMAT, "Was expecting a string; %s",
                  cmp_strerror(&reader->cmp));
    return false;
  }
  if (!ihm_file_read_bytes(reader->fh, &buf, actual_len, err)) return false;
  *match = (actual_len == want_len && strncmp(str, buf, want_len) == 0);
  return true;
}

/* Read the next binary object from the BinaryCIF file and store a copy of it
   at the given pointer. The caller is responsible for freeing it later. */
static bool read_bcif_binary_dup(struct ihm_reader *reader, char **bin,
                                 size_t *bin_size, struct ihm_error **err)
{
  char *buf;
  uint32_t binsz;
  if (!cmp_read_bin_size(&reader->cmp, &binsz)) {
    ihm_error_set(err, IHM_ERROR_FILE_FORMAT, "Was expecting binary; %s",
                  cmp_strerror(&reader->cmp));
    return false;
  }
  if (!ihm_file_read_bytes(reader->fh, &buf, binsz, err)) return false;
  /* memcpy into new buffer */
  free(*bin);
  *bin = ihm_malloc(binsz);
  *bin_size = binsz;
  memcpy(*bin, buf, binsz);
  return true;
}

/* Read the header from a BinaryCIF file to get the number of data blocks */
static bool read_bcif_header(struct ihm_reader *reader, struct ihm_error **err)
{
  uint32_t map_size, i;
  if (!read_bcif_map(reader, &map_size, err)) return false;
  for (i = 0; i < map_size; ++i) {
    bool match;
    if (!read_bcif_exact_string(reader, "dataBlocks", &match,
                                err)) return false;
    if (match) {
      uint32_t array_size;
      if (!read_bcif_array(reader, &array_size, err)) return false;
      reader->num_blocks_left = array_size;
      return true;
    } else {
      if (!skip_bcif_object(reader, err)) return false;
    }
  }
  reader->num_blocks_left = 0;
  return true;
}

/* All valid and supported raw encoder types */
typedef enum {
  BCIF_ENC_NONE,
  BCIF_ENC_STRING_ARRAY,
  BCIF_ENC_BYTE_ARRAY,
  BCIF_ENC_INTEGER_PACKING,
  BCIF_ENC_DELTA,
  BCIF_ENC_RUN_LENGTH,
  BCIF_ENC_FIXED_POINT
} bcif_encoding_kind;

/* An encoding used to compress raw data in BinaryCIF */
struct bcif_encoding {
  /* The encoder type */
  bcif_encoding_kind kind;
  /* Origin (for delta encoding) */
  int32_t origin;
  /* Factor (for fixed point encoding) */
  int32_t factor;
  /* ByteArray type */
  int32_t type;
  /* For integer packing encoding */
  bool is_unsigned;
  /* For integer packing encoding */
  int32_t byte_count;
  /* Encoding of StringArray data */
  struct bcif_encoding *first_data_encoding;
  /* Encoding of StringArray offset */
  struct bcif_encoding *first_offset_encoding;
  /* String data for StringArray encoding */
  char *string_data;
  /* Raw data and size for offsets for StringArray encoding */
  char *offsets;
  size_t offsets_size;
  /* Next encoding, or NULL */
  struct bcif_encoding *next;
};

/* A single column in a BinaryCIF category */
struct bcif_column {
  /* Keyword name */
  char *name;
  /* Raw data and size */
  char *data;
  size_t data_size;
  /* Singly-linked list of data encodings */
  struct bcif_encoding *first_encoding;
  /* Next column, or NULL */
  struct bcif_column *next;
};

/* A single category in a BinaryCIF file */
struct bcif_category {
  /* Category name */
  char *name;
  /* Singly-linked list of column (keyword) information */
  struct bcif_column *first_column;
};

/* Create and return a new bcif_encoding */
static struct bcif_encoding *bcif_encoding_new()
{
  struct bcif_encoding *enc = (struct bcif_encoding *)ihm_malloc(
                                             sizeof(struct bcif_encoding));
  enc->kind = BCIF_ENC_NONE;
  enc->origin = 0;
  enc->factor = 1;
  enc->type = -1;
  enc->is_unsigned = false;
  enc->byte_count = 1;
  enc->first_data_encoding = NULL;
  enc->first_offset_encoding = NULL;
  enc->string_data = NULL;
  enc->offsets = NULL;
  enc->offsets_size = 0;
  enc->next = NULL;
  return enc;
}

/* Free memory used by a bcif_encoding */
static void bcif_encoding_free(struct bcif_encoding *enc)
{
  while(enc->first_data_encoding) {
    struct bcif_encoding *inenc = enc->first_data_encoding;
    enc->first_data_encoding = inenc->next;
    bcif_encoding_free(inenc);
  }
  while(enc->first_offset_encoding) {
    struct bcif_encoding *inenc = enc->first_offset_encoding;
    enc->first_offset_encoding = inenc->next;
    bcif_encoding_free(inenc);
  }
  free(enc->string_data);
  free(enc->offsets);
  free(enc);
}

/* Create and return a new bcif_column */
static struct bcif_column *bcif_column_new()
{
  struct bcif_column *c = (struct bcif_column *)ihm_malloc(
                                             sizeof(struct bcif_column));
  c->name = NULL;
  c->data = NULL;
  c->data_size = 0;
  c->first_encoding = NULL;
  c->next = NULL;
  return c;
}

/* Free memory used by a bcif_column */
static void bcif_column_free(struct bcif_column *col)
{
  free(col->name);
  free(col->data);

  while(col->first_encoding) {
    struct bcif_encoding *enc = col->first_encoding;
    col->first_encoding = enc->next;
    bcif_encoding_free(enc);
  }
  free(col);
}

/* Initialize a new bcif_category */
static void bcif_category_init(struct bcif_category *cat)
{
  cat->name = NULL;
  cat->first_column = NULL;
}

/* Free memory used by a bcif_category */
static void bcif_category_free(struct bcif_category *cat)
{
  free(cat->name);
  while(cat->first_column) {
    struct bcif_column *col = cat->first_column;
    cat->first_column = col->next;
    bcif_column_free(col);
  }
}

static bool read_bcif_encodings(struct ihm_reader *reader,
                                struct bcif_encoding **first,
                                struct ihm_error **err);

/* Read a single encoding from a BinaryCIF file */
static bool read_bcif_encoding(struct ihm_reader *reader,
                               struct bcif_encoding *enc,
                               struct ihm_error **err)
{
  uint32_t map_size, i;
  if (!read_bcif_map(reader, &map_size, err)) return false;
  for (i = 0; i < map_size; ++i) {
    char *str;
    if (!read_bcif_string(reader, &str, err)) return false;
    if (strcmp(str, "kind") == 0) {
      if (!read_bcif_string(reader, &str, err)) return false;
      if (strcmp(str, "StringArray") == 0) {
        enc->kind = BCIF_ENC_STRING_ARRAY;
      } else if (strcmp(str, "ByteArray") == 0) {
        enc->kind = BCIF_ENC_BYTE_ARRAY;
      } else if (strcmp(str, "IntegerPacking") == 0) {
        enc->kind = BCIF_ENC_INTEGER_PACKING;
      } else if (strcmp(str, "Delta") == 0) {
        enc->kind = BCIF_ENC_DELTA;
      } else if (strcmp(str, "RunLength") == 0) {
        enc->kind = BCIF_ENC_RUN_LENGTH;
      } else if (strcmp(str, "FixedPoint") == 0) {
        enc->kind = BCIF_ENC_FIXED_POINT;
      }
    } else if (strcmp(str, "dataEncoding") == 0) {
      if (!read_bcif_encodings(reader,
                               &enc->first_data_encoding, err)) return false;
    } else if (strcmp(str, "offsetEncoding") == 0) {
      if (!read_bcif_encodings(reader,
                               &enc->first_offset_encoding, err)) return false;
    } else if (strcmp(str, "stringData") == 0) {
      if (!read_bcif_string_dup(reader, &enc->string_data, err)) return false;
    } else if (strcmp(str, "offsets") == 0) {
      if (!read_bcif_binary_dup(reader, &enc->offsets,
                                &enc->offsets_size, err)) return false;
    } else if (strcmp(str, "origin") == 0) {
      if (!read_bcif_int(reader, &enc->origin, err)) return false;
    } else if (strcmp(str, "factor") == 0) {
      if (!read_bcif_int(reader, &enc->factor, err)) return false;
    } else if (strcmp(str, "type") == 0) {
      if (!read_bcif_int(reader, &enc->type, err)) return false;
    } else if (strcmp(str, "isUnsigned") == 0) {
      if (!read_bcif_bool(reader, &enc->is_unsigned, err)) return false;
    } else if (strcmp(str, "byteCount") == 0) {
      if (!read_bcif_int(reader, &enc->byte_count, err)) return false;
    } else {
      if (!skip_bcif_object_no_limit(reader, err)) return false;
    }
  }
  return true;
}

/* Read all encoding information from a BinaryCIF file */
static bool read_bcif_encodings(struct ihm_reader *reader,
                                struct bcif_encoding **first,
                                struct ihm_error **err)
{
  uint32_t array_size, i;
  if (!read_bcif_array(reader, &array_size, err)) return false;
  for (i = 0; i < array_size; ++i) {
    struct bcif_encoding *enc = bcif_encoding_new();
    if (!read_bcif_encoding(reader, enc, err)) {
      bcif_encoding_free(enc);
      return false;
    } else {
      enc->next = *first;
      *first = enc;
    }
  }
  return true;
}

/* Read raw data from a BinaryCIF file */
static bool read_bcif_data(struct ihm_reader *reader,
                             struct bcif_column *col,
                             struct ihm_error **err)
{
  uint32_t map_size, i;
  if (!read_bcif_map(reader, &map_size, err)) return false;
  for (i = 0; i < map_size; ++i) {
    char *str;
    if (!read_bcif_string(reader, &str, err)) return false;
    if (strcmp(str, "data") == 0) {
      if (!read_bcif_binary_dup(reader, &col->data,
                                &col->data_size, err)) return false;
    } else if (strcmp(str, "encoding") == 0) {
      if (!read_bcif_encodings(reader, &col->first_encoding, err)) return false;
    } else {
      if (!skip_bcif_object_no_limit(reader, err)) return false;
    }
  }
  return true;
}

/* Read a single column from a BinaryCIF file */
static bool read_bcif_column(struct ihm_reader *reader,
                             struct bcif_column *col,
                             struct ihm_error **err)
{
  uint32_t map_size, i;
  if (!read_bcif_map(reader, &map_size, err)) return false;
  for (i = 0; i < map_size; ++i) {
    char *str;
    if (!read_bcif_string(reader, &str, err)) return false;
    if (strcmp(str, "name") == 0) {
      if (!read_bcif_string_dup(reader, &col->name, err)) return false;
    } else if (strcmp(str, "data") == 0) {
      if (!read_bcif_data(reader, col, err)) return false;
    } else {
      if (!skip_bcif_object_no_limit(reader, err)) return false;
    }
  }
  return true;
}

/* Read all columns for a category from a BinaryCIF file */
static bool read_bcif_columns(struct ihm_reader *reader,
                              struct bcif_category *cat,
                              struct ihm_error **err)
{
  uint32_t array_size, i;
  if (!read_bcif_array(reader, &array_size, err)) return false;
  for (i = 0; i < array_size; ++i) {
    struct bcif_column *col = bcif_column_new();
    if (!read_bcif_column(reader, col, err)) {
      bcif_column_free(col);
      return false;
    } else {
      col->next = cat->first_column;
      cat->first_column = col;
    }
  }
  return true;
}

/* Read a single category from a BinaryCIF file */
static bool read_bcif_category(struct ihm_reader *reader,
                               struct bcif_category *cat,
                               struct ihm_error **err)
{
  uint32_t map_size, i;
  if (!read_bcif_map(reader, &map_size, err)) return false;
  for (i = 0; i < map_size; ++i) {
    char *str;
    if (!read_bcif_string(reader, &str, err)) return false;
    if (strcmp(str, "name") == 0) {
      if (!read_bcif_string_dup(reader, &cat->name, err)) return false;
    } else if (strcmp(str, "columns") == 0) {
      if (!read_bcif_columns(reader, cat, err)) return false;
    } else {
      if (!skip_bcif_object_no_limit(reader, err)) return false;
    }
  }
  return true;
}

/* Read all categories from a BinaryCIF file */
static bool read_bcif_categories(struct ihm_reader *reader,
                                 struct ihm_error **err)
{
  uint32_t ncat, icat;
  if (!read_bcif_array(reader, &ncat, err)) return false;
  for (icat = 0; icat < ncat; ++icat) {
    struct bcif_category cat;
    bcif_category_init(&cat);
    if (!read_bcif_category(reader, &cat, err)) {
      bcif_category_free(&cat);
      return false;
    } else {
      struct bcif_column *col;
      printf("read category %s\n", cat.name);
      for (col = cat.first_column; col; col = col->next) {
        struct bcif_encoding *enc;
        printf("  .%s <data %d", col->name, col->data_size);
	for (enc = col->first_encoding; enc; enc = enc->next) {
          printf(":%s",
                 enc->kind == BCIF_ENC_STRING_ARRAY ? "StringArray" :
                 enc->kind == BCIF_ENC_BYTE_ARRAY ? "ByteArray" :
                 enc->kind == BCIF_ENC_INTEGER_PACKING ? "IntegerPacking" :
                 enc->kind == BCIF_ENC_DELTA ? "Delta" :
                 enc->kind == BCIF_ENC_RUN_LENGTH ? "RunLength" :
                 enc->kind == BCIF_ENC_FIXED_POINT ? "FixedPoint" : "none");
	}
	printf(">\n");
      }
      bcif_category_free(&cat);
    }
  }
  return true;
}

/* Read the next data block from a BinaryCIF file */
static bool read_bcif_block(struct ihm_reader *reader, struct ihm_error **err)
{
  uint32_t map_size, i;
  if (!read_bcif_map(reader, &map_size, err)) return false;
  for (i = 0; i < map_size; ++i) {
    bool match;
    if (!read_bcif_exact_string(reader, "categories", &match,
                                err)) return false;
    if (match) {
      return read_bcif_categories(reader, err);
    } else {
      if (!skip_bcif_object(reader, err)) return false;
    }
  }
  reader->num_blocks_left--;
  return true;
}

/* Read an entire BinaryCIF file. */
static bool read_bcif_file(struct ihm_reader *reader, bool *more_data,
                           struct ihm_error **err)
{
  if (reader->num_blocks_left == -1) {
    cmp_init(&reader->cmp, reader, bcif_cmp_read, bcif_cmp_skip, NULL);
    if (!read_bcif_header(reader, err)) return false;
  }

  if (reader->num_blocks_left > 0) {
    if (!read_bcif_block(reader, err)) return false;
  }
  return reader->num_blocks_left > 0;
}

/* Read an entire mmCIF or BinaryCIF file. */
bool ihm_read_file(struct ihm_reader *reader, bool *more_data,
                   struct ihm_error **err)
{
  if (reader->binary) {
    return read_bcif_file(reader, more_data, err);
  } else {
    return read_mmcif_file(reader, more_data, err);
  }
}
