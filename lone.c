/* SPDX-License-Identifier: AGPL-3.0-or-later */

/* ╭─────────────────────────────┨ LONE LISP ┠──────────────────────────────╮
   │                                                                        │
   │                       The standalone Linux Lisp                        │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
#include <linux/types.h>
#include <linux/unistd.h>
#include <linux/auxvec.h>

typedef __kernel_size_t size_t;
typedef __kernel_ssize_t ssize_t;

static void __attribute__((noreturn)) linux_exit(int code)
{
	system_call_1(__NR_exit, code);
	__builtin_unreachable();
}

static ssize_t linux_read(int fd, const void *buffer, size_t count)
{
	return system_call_3(__NR_read, fd, (long) buffer, (long) count);
}

static ssize_t linux_write(int fd, const void *buffer, size_t count)
{
	return system_call_3(__NR_write, fd, (long) buffer, (long) count);
}

/* ╭────────────────────────────────────────────────────────────────────────╮
   │                                                                        │
   │                                      bits = 32    |    bits = 64       │
   │    digits = ceil(bits * log10(2)) =  10           |    20              │
   │                                                                        │
   │    https://en.wikipedia.org/wiki/FNV_hash                              │
   │    https://datatracker.ietf.org/doc/draft-eastlake-fnv/                │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
#if __BITS_PER_LONG == 64
	#define DECIMAL_DIGITS_PER_LONG 20
	#define FNV_PRIME 0x00000100000001B3UL
	#define FNV_OFFSET_BASIS 0xCBF29CE484222325UL
#elif __BITS_PER_LONG == 32
	#define DECIMAL_DIGITS_PER_LONG 10
	#define FNV_PRIME 0x01000193UL
	#define FNV_OFFSET_BASIS 0x811C9DC5
#else
	#error "Unsupported architecture"
#endif

/* ╭──────────────────────────┨ LONE LISP TYPES ┠───────────────────────────╮
   │                                                                        │
   │    Lone implements dynamic data types as a tagged union.               │
   │    Supported types are:                                                │
   │                                                                        │
   │        ◦ List       the linked list and tree type                      │
   │        ◦ Table      the hash table, prototype and object type          │
   │        ◦ Symbol     the keyword and interned string type               │
   │        ◦ Text       the UTF-8 encoded text type                        │
   │        ◦ Bytes      the binary data and low level string type          │
   │        ◦ Integer    the signed integer type                            │
   │        ◦ Pointer    the memory addressing and dereferencing type       │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
enum lone_type {
	LONE_LIST,
	LONE_TABLE,
	LONE_SYMBOL,
	LONE_TEXT,
	LONE_BYTES,
	LONE_INTEGER,
	LONE_POINTER,
};

struct lone_bytes {
	size_t count;
	unsigned char *pointer;
};

struct lone_list {
	struct lone_value *first;
	struct lone_value *rest;
};

struct lone_table_entry {
	struct lone_value *key;
	struct lone_value *value;
};

struct lone_table {
	size_t count;
	size_t capacity;
	struct lone_table_entry *entries;
	struct lone_value *prototype;
};

struct lone_value {
	enum lone_type type;
	union {
		struct lone_list list;
		struct lone_table table;
		struct lone_bytes bytes;   /* also used by texts and symbols */
		long integer;
		void *pointer;
	};
};

/* ╭───────────────────────┨ LONE LISP INTERPRETER ┠────────────────────────╮
   │                                                                        │
   │    The lone lisp interpreter is composed of all internal state         │
   │    necessary to process useful programs. It includes memory,           │
   │    the top level lisp environment and references to all objects.       │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
struct lone_memory;
struct lone_lisp {
	struct lone_memory *memory;
	struct lone_values *values;
	struct lone_value *environment;
};

/* ╭────────────────────┨ LONE LISP MEMORY ALLOCATION ┠─────────────────────╮
   │                                                                        │
   │    Lone is designed to work without any dependencies except Linux,     │
   │    so it does not make use of even the system's C library.             │
   │    In order to bootstrap itself in such harsh conditions,              │
   │    it must be given some memory to work with.                          │
   │                                                                        │
   │    Lone manages its own memory with a block-based allocator.           │
   │    Memory blocks are allocated on a first fit basis.                   │
   │    They will be split into smaller units when allocated                │
   │    and merged together with free neighbors when deallocated.           │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
struct lone_memory {
	struct lone_memory *prev, *next;
	int free;
	size_t size;
	unsigned char pointer[];
};

struct lone_values {
	struct lone_values *next;
	struct lone_value value;
};

static void lone_memory_move(void *from, void *to, size_t count)
{
	unsigned char *source = from, *destination = to;

	if (source >= destination) {
		/* destination is at or behind source, copy forwards */
		while (count--) { *destination++ = *source++; }
	} else {
		/* destination is ahead of source, copy backwards */
		source += count; destination += count;
		while (count--) { *--destination = *--source; }
	}
}

static void lone_memory_split(struct lone_memory *block, size_t used)
{
	size_t excess = block->size - used;

	/* split block if there's enough space to allocate at least 1 byte */
	if (excess >= sizeof(struct lone_memory) + 1) {
		struct lone_memory *new = (struct lone_memory *) (block->pointer + used);
		new->next = block->next;
		new->prev = block;
		new->free = 1;
		new->size = excess - sizeof(struct lone_memory);
		block->next = new;
		block->size -= excess;
	}
}

static void lone_memory_coalesce(struct lone_memory *block)
{
	struct lone_memory *next;

	if (block && block->free) {
		next = block->next;
		if (next && next->free) {
			block->size += next->size + sizeof(struct lone_memory);
			next = block->next = next->next;
			if (next) { next->prev = block; }
		}
	}
}

static void *lone_allocate(struct lone_lisp *lone, size_t requested_size)
{
	size_t needed_size = requested_size + sizeof(struct lone_memory);
	struct lone_memory *block;

	for (block = lone->memory; block; block = block->next) {
		if (block->free && block->size >= needed_size)
			break;
	}

	if (!block) { linux_exit(-1); }

	block->free = 0;
	lone_memory_split(block, needed_size);

	return block->pointer;
}

static void lone_deallocate(struct lone_lisp *lone, void * pointer)
{
	struct lone_memory *block = ((struct lone_memory *) pointer) - 1;
	block->free = 1;

	lone_memory_coalesce(block);
	lone_memory_coalesce(block->prev);
}

static void lone_deallocate_all(struct lone_lisp *lone)
{
	struct lone_values *value = lone->values, *next;

	while (value) {
		next = value->next;

		switch (value->value.type) {
		case LONE_SYMBOL:
		case LONE_TEXT:
		case LONE_BYTES:
			lone_deallocate(lone, value->value.bytes.pointer);
			break;
		}

		lone_deallocate(lone, value);

		value = next;
	}

	lone->values = 0;
}

static void *lone_reallocate(struct lone_lisp *lone, void *pointer, size_t size)
{
	struct lone_memory *old = ((struct lone_memory *) pointer) - 1,
	                   *new = ((struct lone_memory *) lone_allocate(lone, size)) - 1;

	if (pointer) {
		lone_memory_move(old->pointer, new->pointer, new->size < old->size ? new->size : old->size);
		lone_deallocate(lone, pointer);
	}

	return new->pointer;
}

/* ╭────────────────────────────────────────────────────────────────────────╮
   │                                                                        │
   │    Initializers and creation functions for lone's types.               │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static void lone_lisp_initialize(struct lone_lisp *lone, unsigned char *memory, size_t size)
{
	lone->memory = (struct lone_memory *) memory;
	lone->memory->prev = lone->memory->next = 0;
	lone->memory->free = 1;
	lone->memory->size = size - sizeof(struct lone_memory);
	lone->values = 0;
	lone->environment = 0;
}

static struct lone_value *lone_value_create(struct lone_lisp *lone)
{
	struct lone_values *values = lone_allocate(lone, sizeof(struct lone_values));
	if (lone->values) { values->next = lone->values; }
	lone->values = values;
	return &values->value;
}

static struct lone_value *lone_bytes_create(struct lone_lisp *lone, unsigned char *pointer, size_t count)
{
	struct lone_value *value = lone_value_create(lone);
	unsigned char *copy = lone_allocate(lone, count);
	lone_memory_move(pointer, copy, count);
	value->type = LONE_BYTES;
	value->bytes.count = count;
	value->bytes.pointer = copy;
	return value;
}

static struct lone_value *lone_list_create(struct lone_lisp *lone, struct lone_value *first, struct lone_value *rest)
{
	struct lone_value *value = lone_value_create(lone);
	value->type = LONE_LIST;
	value->list.first = first;
	value->list.rest = rest;
	return value;
}

static struct lone_value *lone_list_create_nil(struct lone_lisp *lone)
{
	return lone_list_create(lone, 0, 0);
}

static struct lone_value *lone_table_create(struct lone_lisp *lone, size_t capacity, struct lone_value *prototype)
{
	struct lone_value *value = lone_value_create(lone);
	value->type = LONE_TABLE;
	value->table.prototype = prototype;
	value->table.capacity = capacity;
	value->table.count = 0;
	value->table.entries = lone_allocate(lone, capacity * sizeof(*value->table.entries));

	for (size_t i = 0; i < capacity; ++i) {
		value->table.entries[i].key = 0;
		value->table.entries[i].value = 0;
	}

	return value;
}

static struct lone_value *lone_integer_create(struct lone_lisp *lone, long integer)
{
	struct lone_value *value = lone_value_create(lone);
	value->type = LONE_INTEGER;
	value->integer = integer;
	return value;
}

static struct lone_value *lone_integer_parse(struct lone_lisp *lone, unsigned char *digits, size_t count)
{
	size_t i = 0;
	long integer = 0;

	switch (*digits) { case '+': case '-': ++i; break; }

	while (i < count) {
		integer *= 10;
		integer += digits[i++] - '0';
	}

	if (*digits == '-') { integer *= -1; }

	return lone_integer_create(lone, integer);
}

static struct lone_value *lone_pointer_create(struct lone_lisp *lone, void *pointer)
{
	struct lone_value *value = lone_value_create(lone);
	value->type = LONE_POINTER;
	value->pointer = pointer;
	return value;
}

static struct lone_value *lone_text_create(struct lone_lisp *lone, unsigned char *text, size_t length)
{
	struct lone_value *value = lone_bytes_create(lone, text, length);
	value->type = LONE_TEXT;
	return value;
}

static size_t lone_c_string_length(char *c_string)
{
	size_t length = 0;
	if (!c_string) { return 0; }
	while (c_string[length++]);
	return length;
}

static struct lone_value *lone_text_create_from_c_string(struct lone_lisp *lone, char *c_string)
{
	return lone_text_create(lone, (unsigned char *) c_string, lone_c_string_length(c_string) - 1);
}

static struct lone_value *lone_symbol_create(struct lone_lisp *lone, unsigned char *text, size_t length)
{
	struct lone_value *value = lone_bytes_create(lone, text, length);
	value->type = LONE_SYMBOL;
	return value;
}

static struct lone_value *lone_symbol_create_from_c_string(struct lone_lisp *lone, char *c_string)
{
	return lone_symbol_create(lone, (unsigned char*) c_string, lone_c_string_length(c_string) - 1);
}

/* ╭────────────────────────────────────────────────────────────────────────╮
   │                                                                        │
   │    Functions for operating on lone's built-in types.                   │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static int lone_is_nil(struct lone_value *value)
{
	return value->type == LONE_LIST && value->list.first == 0 && value->list.rest == 0;
}

static struct lone_value *lone_list_set(struct lone_value *list, struct lone_value *value)
{
	return list->list.first = value;
}

static struct lone_value *lone_list_last(struct lone_value *list)
{
	while (!lone_is_nil(list->list.rest)) { list = list->list.rest; }
	return list;
}

static struct lone_value *lone_list_append(struct lone_value *list, struct lone_value *rest)
{
	return list->list.rest = rest;
}

static struct lone_value *lone_list_pop(struct lone_value **list)
{
	struct lone_value *value = (*list)->list.first;
	*list = (*list)->list.rest;
	return value;
}

static int lone_bytes_equals(struct lone_bytes x, struct lone_bytes y)
{
	if (x.count != y.count) return 0;
	for (size_t i = 0; i < x.count; ++i) if (x.pointer[i] != y.pointer[i]) return 0;
	return 1;
}

static unsigned long fnv_1a(unsigned char *bytes, size_t count)
{
	unsigned long hash = FNV_OFFSET_BASIS;

	while (count--) {
		hash ^= *bytes++;
		hash *= FNV_PRIME;
	}

	return hash;
}

static inline size_t lone_table_compute_hash_for(struct lone_value *key, size_t capacity)
{
	return fnv_1a(key->bytes.pointer, key->bytes.count) % capacity;
}

static size_t lone_table_entry_find_index_for(struct lone_value *key, struct lone_table_entry *entries, size_t capacity)
{
	size_t i = lone_table_compute_hash_for(key, capacity);
	struct lone_table_entry *entry;

	while (1) {
		entry = &entries[i];
		if (!entry->key || lone_bytes_equals(entry->key->bytes, key->bytes)) { return i; }
		i = i < capacity ? i + 1 : 0;
	}
}

static int lone_table_entry_set(struct lone_table_entry *entries, size_t capacity, struct lone_value *key, struct lone_value *value)
{
	size_t i = lone_table_entry_find_index_for(key, entries, capacity);
	struct lone_table_entry *entry = &entries[i];

	if (entry->key) {
		entry->value = value;
		return 0;
	} else {
		entry->key = key;
		entry->value = value;
		return 1;
	}
}

static void lone_table_resize(struct lone_lisp *lone, struct lone_value *table, size_t new_capacity)
{
	size_t old_capacity = table->table.capacity, i;
	struct lone_table_entry *old = table->table.entries,
	                        *new = lone_allocate(lone, new_capacity * sizeof(*new));

	for (i = 0; i < new_capacity; ++i) {
		new[i].key = 0;
		new[i].value = 0;
	}

	for (i = 0; i < old_capacity; ++i) {
		if (old[i].key) {
			lone_table_entry_set(new, new_capacity, old[i].key, old[i].value);
		}
	}

	lone_deallocate(lone, old);
	table->table.entries = new;
	table->table.capacity = new_capacity;
}

static void lone_table_set(struct lone_lisp *lone, struct lone_value *table, struct lone_value *key, struct lone_value *value)
{
	struct lone_table_entry *entries = table->table.entries;
	size_t count = table->table.count, capacity = table->table.capacity;

	if (count >= capacity / 2) {
		lone_table_resize(lone, table, capacity * 2);
	}

	if (lone_table_entry_set(entries, capacity, key, value)) { ++table->table.count; }
}

static struct lone_value *lone_table_get(struct lone_lisp *lone, struct lone_value *table, struct lone_value *key)
{
	size_t capacity = table->table.capacity, i;
	struct lone_table_entry *entries = table->table.entries, *entry;
	struct lone_value *prototype = table->table.prototype;

	i = lone_table_entry_find_index_for(key, entries, capacity);
	entry = &entries[i];

	if (entry->key) {
		return entry->value;
	} else if (prototype && !lone_is_nil(prototype)) {
		return lone_table_get(lone, prototype, key);
	} else {
		return lone_list_create_nil(lone);
	}
}

static void lone_table_delete(struct lone_lisp *lone, struct lone_value *table, struct lone_value *key)
{
	size_t capacity = table->table.capacity, i, j, k;
	struct lone_table_entry *entries = table->table.entries;

	i = lone_table_entry_find_index_for(key, entries, capacity);

	if (!entries[i].key) { return; }

	j = i;
	while (1) {
		j = (j + 1) % capacity;
		if (!entries[j].key) { break; }
		k = lone_table_compute_hash_for(entries[j].key, capacity);
		if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) {
			entries[i] = entries[j];
			i = j;
		}
	}

	entries[i].key = 0;
	entries[i].value = 0;
	--table->table.count;
}


/* ╭─────────────────────────┨ LONE LISP READER ┠───────────────────────────╮
   │                                                                        │
   │    The reader's job is to transform input into lone lisp values.       │
   │    It accomplishes the task by reading input from a given file         │
   │    descriptor and then parsing the results.                            │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
struct lone_reader {
	int file_descriptor;
	struct lone_bytes buffer;
	size_t position;
	struct lone_value *remaining_tokens;
	int finished;
};

static void lone_reader_initialize(struct lone_lisp *lone, struct lone_reader *reader, size_t buffer_size, int file_descriptor)
{
	reader->file_descriptor = file_descriptor;
	reader->buffer.count = buffer_size;
	reader->buffer.pointer = lone_allocate(lone, buffer_size);
	reader->position = 0;
	reader->remaining_tokens = 0;
	reader->finished = 0;
}

/* ╭──────────────────────────┨ LONE LISP LEXER ┠───────────────────────────╮
   │                                                                        │
   │    The lexer or tokenizer transforms a linear stream of characters     │
   │    into a linear stream of tokens suitable for parser consumption.     │
   │    This gets rid of insignificant whitespace and reduces the size      │
   │    of the parser's input significantly.                                │
   │                                                                        │
   │    It consists of an input buffer, its current position in it          │
   │    as well as two functions:                                           │
   │                                                                        │
   │        ◦ peek(k) which returns the character at i+k                    │
   │        ◦ consume(k) which advances i by k positions                    │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
struct lone_lexer {
	struct lone_bytes input;
	size_t position;
};

/* ╭────────────────────────────────────────────────────────────────────────╮
   │                                                                        │
   │    The peek(k) function returns the k-th element from the input        │
   │    starting from the current input position, with peek(0) being        │
   │    the current character and peek(k) being look ahead for k > 1.       │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static unsigned char *lone_lexer_peek_k(struct lone_lexer *lexer, size_t k)
{
	if (lexer->position + k >= lexer->input.count)
		return 0;
	return lexer->input.pointer + lexer->position + k;
}

static unsigned char *lone_lexer_peek(struct lone_lexer *lexer)
{
	return lone_lexer_peek_k(lexer, 0);
}

/* ╭────────────────────────────────────────────────────────────────────────╮
   │                                                                        │
   │    The consume(k) function advances the input position by k.           │
   │    This progresses through the input, consuming it.                    │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static void lone_lexer_consume_k(struct lone_lexer *lexer, size_t k)
{
	lexer->position += k;
}

static void lone_lexer_consume(struct lone_lexer *lexer)
{
	lone_lexer_consume_k(lexer, 1);
}

static int lone_lexer_match_byte(unsigned char byte, unsigned char target)
{
	if (target == ' ') {
		switch (byte) {
		case ' ':
		case '\t':
		case '\n':
			return 1;
		default:
			return 0;
		}
	} else if (target >= '0' && target <= '9') {
		return byte >= '0' && byte <= '9';
	} else {
		return byte == target;
	}
}

/* ╭────────────────────────────────────────────────────────────────────────╮
   │                                                                        │
   │    Analyzes a number and adds it to the tokens list if valid.          │
   │                                                                        │
   │    ([+-]?[0-9]+)[) \n\t]                                               │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static int lone_lexer_consume_number(struct lone_lisp *lone, struct lone_lexer *lexer, struct lone_value *list)
{
	unsigned char *current, *start = lone_lexer_peek(lexer);
	if (!start) { return 1; }
	size_t end = 0;

	switch (*start) {
	case '+': case '-':
		lone_lexer_consume(lexer);
		++end;
		break;
	default:
		break;
	}

	if ((current = lone_lexer_peek(lexer)) && lone_lexer_match_byte(*current, '1')) {
		lone_lexer_consume(lexer);
		++end;
	} else { return 1; }

	while ((current = lone_lexer_peek(lexer)) && lone_lexer_match_byte(*current, '1')) {
		lone_lexer_consume(lexer);
		++end;
	}

	if (current && *current != ')' && !lone_lexer_match_byte(*current, ' ')) { return 1; }

	lone_list_set(list, lone_integer_parse(lone, start, end));
	return 0;

}

/* ╭────────────────────────────────────────────────────────────────────────╮
   │                                                                        │
   │    Analyzes a symbol and adds it to the tokens list if valid.          │
   │                                                                        │
   │    (.*)[) \n\t]                                                        │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static int lone_lexer_consume_symbol(struct lone_lisp *lone, struct lone_lexer *lexer, struct lone_value *list)
{
	unsigned char *current, *start = lone_lexer_peek(lexer);
	if (!start) { return 1; }
	size_t end = 0;

	while ((current = lone_lexer_peek(lexer)) && *current != ')' && !lone_lexer_match_byte(*current, ' ')) {
		lone_lexer_consume(lexer);
		++end;
	}

	lone_list_set(list, lone_symbol_create(lone, start, end));
	return 0;
}

/* ╭────────────────────────────────────────────────────────────────────────╮
   │                                                                        │
   │    Analyzes a string and adds it to the tokens list if valid.          │
   │                                                                        │
   │    (".*")[) \n\t]                                                      │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static int lone_lexer_consume_text(struct lone_lisp *lone, struct lone_lexer *lexer, struct lone_value *list)
{
	size_t end = 0;
	unsigned char *current, *start = lone_lexer_peek(lexer);
	if (!start || *start != '"') { return 1; }

	// skip leading "
	++start;
	lone_lexer_consume(lexer);

	while ((current = lone_lexer_peek(lexer)) && *current != '"') {
		lone_lexer_consume(lexer);
		++end;
	}

	// skip trailing "
	++current;
	lone_lexer_consume(lexer);

	if (*current != ')' && !lone_lexer_match_byte(*current, ' ')) { return 1; }

	lone_list_set(list, lone_text_create(lone, start, end));
	return 0;
}

/* ╭────────────────────────────────────────────────────────────────────────╮
   │                                                                        │
   │    Analyzes opening and closing parentheses                            │
   │    and adds them to the tokens list if valid.                          │
   │                                                                        │
   │    ([()])                                                              │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static int lone_lexer_consume_parenthesis(struct lone_lisp *lone, struct lone_lexer *lexer, struct lone_value *list)
{
	unsigned char *parenthesis = lone_lexer_peek(lexer);
	if (!parenthesis) { return 1; }

	switch (*parenthesis) {
	case '(': case ')':
		lone_list_set(list, lone_symbol_create(lone, parenthesis, 1));
		lone_lexer_consume(lexer);
		break;
	default: return 1;
	}

	return 0;
}

/* ╭────────────────────────────────────────────────────────────────────────╮
   │                                                                        │
   │    The lone lisp lexer receives as input a single lone bytes value     │
   │    containing the full source code to be processed and it outputs      │
   │    a lone list of each lisp token found in the input. For example:     │
   │                                                                        │
   │        lex ← lone_bytes = [ (abc ("zxc") ]                             │
   │        lex → lone_list  = { ( → abc → ( → "zxc" → ) }                  │
   │                                                                        │
   │    Note that the list is linear and parentheses are not matched.       │
   │    The lexical analysis algorithm can be summarized as follows:        │
   │                                                                        │
   │        ◦ Skip all whitespace until it finds something                  │
   │        ◦ Fail if tokens aren't separated by spaces or ) at the end     │
   │        ◦ If found sign before digits tokenize signed number            │
   │        ◦ If found digit then look for more digits and tokenize         │
   │        ◦ If found " then find the next " and tokenize                  │
   │        ◦ If found ( or ) just tokenize them as is without matching     │
   │        ◦ Tokenize everything else unmodified as a symbol               │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static struct lone_value *lone_lex(struct lone_lisp *lone, struct lone_lexer *lexer)
{
	struct lone_value *first = lone_list_create_nil(lone), *current = first;
	unsigned char *c;

	while ((c = lone_lexer_peek(lexer))) {
		if (lone_lexer_match_byte(*c, ' ')) {
			lone_lexer_consume(lexer);
			continue;
		} else {
			unsigned char *c1;
			int failed = 1;

			switch (*c) {
			case '+': case '-':
				if ((c1 = lone_lexer_peek_k(lexer, 1)) && lone_lexer_match_byte(*c1, '1')) {
					failed = lone_lexer_consume_number(lone, lexer, current);
				} else {
					failed = lone_lexer_consume_symbol(lone, lexer, current);
				}
				break;
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				failed = lone_lexer_consume_number(lone, lexer, current);
				break;
			case '"':
				failed = lone_lexer_consume_text(lone, lexer, current);
				break;
			case '(':
			case ')':
				failed = lone_lexer_consume_parenthesis(lone, lexer, current);
				break;
			default:
				failed = lone_lexer_consume_symbol(lone, lexer, current);
				break;
			}

			if (failed) {
				goto lex_failed;
			}

			current = lone_list_append(current, lone_list_create_nil(lone));
		}
	}

	return first;

lex_failed:
	linux_exit(-1);
}

/* ╭─────────────────────────┨ LONE LISP PARSER ┠───────────────────────────╮
   │                                                                        │
   │    The parser transforms a linear sequence of tokens into a nested     │
   │    sequence of lisp objects suitable for evaluation.                   │
   │    Its main task is to match nested structures such as lists.          │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static struct lone_value *lone_parse_tokens(struct lone_lisp *, struct lone_value **);

static struct lone_value *lone_parse_list(struct lone_lisp *lone, struct lone_value **tokens)
{
	struct lone_value *list = lone_list_create_nil(lone), *first = list;

	while (1) {
		if (lone_is_nil(*tokens)) {
			// expected token or ) but found end of input
			return 0;
		}

		struct lone_value *current = (*tokens)->list.first;

		if (current->type == LONE_SYMBOL && *current->bytes.pointer == ')') {
			lone_list_pop(tokens);
			break;
		}

		lone_list_set(list, lone_parse_tokens(lone, tokens));
		list = lone_list_append(list, lone_list_create_nil(lone));
	}

	return first;
}

static struct lone_value *lone_parse_tokens(struct lone_lisp *lone, struct lone_value **tokens)
{
	if (lone_is_nil(*tokens)) { return *tokens; }
	if ((*tokens)->list.first == 0) { goto parse_failed; }
	struct lone_value *token = lone_list_pop(tokens);

	/* lexer may already have parsed some types */
	switch (token->type) {
	case LONE_SYMBOL:
		switch (*token->bytes.pointer) {
		case '(':
			return lone_parse_list(lone, tokens);
		case ')':
			goto parse_failed;
		default:
			return token;
		}
	case LONE_INTEGER:
	case LONE_TEXT:
		return token;
	default:
		linux_exit(-1);
	}

parse_failed:
	linux_exit(-1);
}

static struct lone_value *lone_parse(struct lone_lisp *lone, struct lone_value *value, struct lone_value **remainder)
{
	struct lone_lexer lexer = { value->bytes, 0 };
	struct lone_value *tokens = lone_lex(lone, &lexer), *parsed;

	if (*remainder && !lone_is_nil(*remainder)) {
		lone_list_append(lone_list_last(*remainder), tokens);
		tokens = *remainder;
	}

	parsed = lone_parse_tokens(lone, &tokens);

	*remainder = tokens;
	return parsed;
}

static size_t lone_read_from_file_descriptor(struct lone_lisp *lone, struct lone_reader *reader)
{
	unsigned char *buffer = reader->buffer.pointer;
	size_t size = reader->buffer.count, position = reader->position, allocated = size, bytes_read = 0, total_read = 0;
	ssize_t read_result = 0;

	while (1) {
		read_result = linux_read(reader->file_descriptor, buffer + position, size);

		if (read_result < 0) {
			linux_exit(-1);
		}

		bytes_read = (size_t) read_result;
		total_read += bytes_read;
		position += bytes_read;

		if (bytes_read == size) {
			allocated += size;
			buffer = lone_reallocate(lone, buffer, allocated);
		} else {
			break;
		}
	}

	reader->buffer.pointer = buffer;
	reader->buffer.count = allocated;
	reader->position = position;
	return total_read;
}

static struct lone_value *lone_read(struct lone_lisp *lone, struct lone_reader *reader)
{
	struct lone_value *value;
	size_t bytes_read;

	do {
		bytes_read = lone_read_from_file_descriptor(lone, reader);
		value = lone_bytes_create(lone, reader->buffer.pointer, reader->position);
		value = lone_parse(lone, value, &reader->remaining_tokens);

		if (bytes_read == 0) {
			// there was no more input
			if (value == 0) {
				// the parser wanted more bytes
				return 0;
			} else if (lone_is_nil(value)) {
				// the parser consumed all input
				reader->finished = 1;
			}
		}

	} while (value == 0);

	// successfully read a value, reset reader position and return it
	reader->position = 0;
	return value;
}

/* ╭────────────────────────┨ LONE LISP EVALUATOR ┠─────────────────────────╮
   │                                                                        │
   │    The heart of the language. This is what actually executes code.     │
   │    Currently supports resolving variable references.                   │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static struct lone_value *lone_evaluate(struct lone_lisp *lone, struct lone_value *value)
{
	if (value == 0) { return 0; }

	switch (value->type) {
	case LONE_BYTES:
	case LONE_LIST:
	case LONE_TABLE:
	case LONE_INTEGER:
	case LONE_POINTER:
	case LONE_TEXT:
		return value;
		break;
	case LONE_SYMBOL:
		return lone_table_get(lone, lone->environment, value);
	}
}

/* ╭─────────────────────────┨ LONE LISP PRINTER ┠──────────────────────────╮
   │                                                                        │
   │    Transforms lone lisp objects into text in order to write it out.    │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static void lone_print_integer(int fd, long n)
{
	static char digits[DECIMAL_DIGITS_PER_LONG + 1]; /* digits, sign */
	char *digit = digits + DECIMAL_DIGITS_PER_LONG;  /* work backwards */
	size_t count = 0;
	int is_negative;

	if (n < 0) {
		is_negative = 1;
		n *= -1;
	} else {
		is_negative = 0;
	}

	do {
		*--digit = '0' + (n % 10);
		n /= 10;
		++count;
	} while (n > 0);

	if (is_negative) {
		*--digit = '-';
		++count;
	}

	linux_write(fd, digit, count);
}

static void lone_print_bytes(struct lone_lisp *lone, struct lone_value *bytes, int fd)
{
	size_t count = bytes->bytes.count;
	if (count == 0) { linux_write(fd, "bytes[]", 7); return; }

	static unsigned char hexadecimal[] = "0123456789ABCDEF";
	size_t size = 2 + count * 2; // size required: "0x" + 2 characters per input byte
	unsigned char *text = lone_allocate(lone, size);
	unsigned char *byte = bytes->bytes.pointer;
	size_t i;

	text[0] = '0';
	text[1] = 'x';

	for (i = 0; i < count; ++i) {
		unsigned char low  = (byte[i] & 0x0F) >> 0;
		unsigned char high = (byte[i] & 0xF0) >> 4;
		text[2 + (2 * i + 0)] = hexadecimal[high];
		text[2 + (2 * i + 1)] = hexadecimal[low];
	}

	linux_write(fd, "bytes[", 6);
	linux_write(fd, text, size);
	linux_write(fd, "]", 1);

	lone_deallocate(lone, text);
}

static void lone_print(struct lone_lisp *, struct lone_value *, int);

static void lone_print_list(struct lone_lisp *lone, struct lone_value *list, int fd)
{
	if (list == 0 || lone_is_nil(list)) { return; }

	struct lone_value *first = list->list.first,
	                  *rest  = list->list.rest;

	lone_print(lone, first, fd);

	if (rest->type == LONE_LIST) {
		if (!lone_is_nil(rest)) {
			linux_write(fd, " ", 1);
			lone_print_list(lone, rest, fd);
		}
	} else {
		linux_write(fd, " . ", 3);
		lone_print(lone, rest, fd);
	}
}

static void lone_print_table(struct lone_lisp *lone, struct lone_value *table, int fd)
{
	size_t n = table->table.capacity, i;
	struct lone_table_entry *entries = table->table.entries;

	linux_write(fd, "{ ", 2);

	for (i = 0; i < n; ++i) {
		struct lone_value *key   = entries[i].key,
		                  *value = entries[i].value;


		if (key) {
			lone_print(lone, entries[i].key, fd);
			linux_write(fd, " ", 1);
			if (value) { lone_print(lone, entries[i].value, fd); }
			else { linux_write(fd, "nil", 3); }
			linux_write(fd, " ", 1);
		}
	}

	linux_write(fd, "}", 1);
}

static void lone_print(struct lone_lisp *lone, struct lone_value *value, int fd)
{
	if (value == 0 || lone_is_nil(value)) { return; }

	switch (value->type) {
	case LONE_LIST:
		linux_write(fd, "(", 1);
		lone_print_list(lone, value, fd);
		linux_write(fd, ")", 1);
		break;
	case LONE_TABLE:
		lone_print_table(lone, value, fd);
		break;
	case LONE_BYTES:
		lone_print_bytes(lone, value, fd);
		break;
	case LONE_SYMBOL:
		linux_write(fd, value->bytes.pointer, value->bytes.count);
		break;
	case LONE_TEXT:
		linux_write(fd, "\"", 1);
		linux_write(fd, value->bytes.pointer, value->bytes.count);
		linux_write(fd, "\"", 1);
		break;
	case LONE_INTEGER:
	case LONE_POINTER:
		lone_print_integer(fd, value->integer);
		break;
	}
}

/* ╭─────────────────────────┨ LONE LINUX PROCESS ┠─────────────────────────╮
   │                                                                        │
   │    Code to access all the parameters Linux passes to its processes.    │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
struct auxiliary {
	long type;
	union {
		char *c_string;
		void *pointer;
		long integer;
	} as;
};

static void lone_auxiliary_value_to_table(struct lone_lisp *lone, struct lone_value *table, struct auxiliary *auxiliary_value)
{
	struct lone_value *key, *value;
	switch (auxiliary_value->type) {
	case AT_BASE_PLATFORM:
		key = lone_symbol_create_from_c_string(lone, "base-platform");
		value = lone_text_create_from_c_string(lone, auxiliary_value->as.c_string);
		break;
	case AT_PLATFORM:
		key = lone_symbol_create_from_c_string(lone, "platform");
		value = lone_text_create_from_c_string(lone, auxiliary_value->as.c_string);
		break;
	case AT_HWCAP:
		key = lone_symbol_create_from_c_string(lone, "hardware-capabilities");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
	case AT_HWCAP2:
		key = lone_symbol_create_from_c_string(lone, "hardware-capabilities-2");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
	case AT_FLAGS:
		key = lone_symbol_create_from_c_string(lone, "flags");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
	case AT_NOTELF:
		key = lone_symbol_create_from_c_string(lone, "not-ELF");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
	case AT_BASE:
		key = lone_symbol_create_from_c_string(lone, "interpreter-base-address");
		value = lone_pointer_create(lone, auxiliary_value->as.pointer);
		break;
	case AT_ENTRY:
		key = lone_symbol_create_from_c_string(lone, "entry-point");
		value = lone_pointer_create(lone, auxiliary_value->as.pointer);
		break;
	case AT_SYSINFO_EHDR:
		key = lone_symbol_create_from_c_string(lone, "vDSO");
		value = lone_pointer_create(lone, auxiliary_value->as.pointer);
		break;
	case AT_PHDR:
		key = lone_symbol_create_from_c_string(lone, "program-headers-address");
		value = lone_pointer_create(lone, auxiliary_value->as.pointer);
		break;
	case AT_PHENT:
		key = lone_symbol_create_from_c_string(lone, "program-headers-entry-size");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
	case AT_PHNUM:
		key = lone_symbol_create_from_c_string(lone, "program-headers-count");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
	case AT_EXECFN:
		key = lone_symbol_create_from_c_string(lone, "executable-file-name");
		value = lone_text_create_from_c_string(lone, auxiliary_value->as.c_string);
		break;
	case AT_EXECFD:
		key = lone_symbol_create_from_c_string(lone, "executable-file-descriptor");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
	case AT_UID:
		key = lone_symbol_create_from_c_string(lone, "user-id");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
	case AT_EUID:
		key = lone_symbol_create_from_c_string(lone, "effective-user-id");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
	case AT_GID:
		key = lone_symbol_create_from_c_string(lone, "group-id");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
	case AT_EGID:
		key = lone_symbol_create_from_c_string(lone, "effective-group-id");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
	case AT_PAGESZ:
		key = lone_symbol_create_from_c_string(lone, "page-size");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
#ifdef AT_MINSIGSTKSZ
	case AT_MINSIGSTKSZ:
		key = lone_symbol_create_from_c_string(lone, "minimum-signal-delivery-stack-size");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
#endif
	case AT_CLKTCK:
		key = lone_symbol_create_from_c_string(lone, "clock-tick");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
	case AT_RANDOM:
		key = lone_symbol_create_from_c_string(lone, "random");
		value = lone_bytes_create(lone, auxiliary_value->as.pointer, 16);
		break;
	case AT_SECURE:
		key = lone_symbol_create_from_c_string(lone, "secure");
		value = lone_integer_create(lone, auxiliary_value->as.integer);
		break;
	default:
		key = lone_symbol_create_from_c_string(lone, "unknown");
		value = lone_list_create(lone,
		                         lone_integer_create(lone, auxiliary_value->type),
		                         lone_integer_create(lone, auxiliary_value->as.integer));
	}

	lone_table_set(lone, table, key, value);
}

static struct lone_value *lone_auxiliary_vector_to_table(struct lone_lisp *lone, struct auxiliary *auxiliary_values)
{
	struct lone_value *table = lone_table_create(lone, 32, 0);
	size_t i;

	for (i = 0; auxiliary_values[i].type != AT_NULL; ++i) {
		lone_auxiliary_value_to_table(lone, table, &auxiliary_values[i]);
	}

	return table;
}

static struct lone_value *lone_environment_to_table(struct lone_lisp *lone, char **c_strings)
{
	struct lone_value *table = lone_table_create(lone, 64, 0), *key, *value;
	char *c_string_key, *c_string_value, *c_string;

	for (/* c_strings */; *c_strings; ++c_strings) {
		c_string = *c_strings;
		c_string_key = c_string;
		c_string_value = "";

		while (*c_string++) {
			if (*c_string == '=') {
				*c_string = '\0';
				c_string_value = c_string + 1;
				break;
			}
		}

		key = lone_text_create_from_c_string(lone, c_string_key);
		value = lone_text_create_from_c_string(lone, c_string_value);
		lone_table_set(lone, table, key, value);
	}

	return table;
}

static struct lone_value *lone_arguments_to_list(struct lone_lisp *lone, int count, char **c_strings)
{
	struct lone_value *arguments = lone_list_create_nil(lone), *head;
	int i;

	for (i = 0, head = arguments; i < count; ++i) {
		lone_list_set(head, lone_text_create_from_c_string(lone, c_strings[i]));
		head = lone_list_append(head, lone_list_create_nil(lone));
	}

	return arguments;
}

static void lone_set_environment(struct lone_lisp *lone, struct lone_value *arguments, struct lone_value *environment, struct lone_value *auxiliary_values)
{
	struct lone_value *table = lone_table_create(lone, 16, 0);

	lone_table_set(lone, table, lone_symbol_create_from_c_string(lone, "arguments"), arguments);
	lone_table_set(lone, table, lone_symbol_create_from_c_string(lone, "environment"), environment);
	lone_table_set(lone, table, lone_symbol_create_from_c_string(lone, "auxiliary-values"), auxiliary_values);

	lone->environment = table;
}

/* ╭───────────────────────┨ LONE LISP ENTRY POINT ┠────────────────────────╮
   │                                                                        │
   │    Linux places argument, environment and auxiliary value arrays       │
   │    on the stack before jumping to the entry point of the process.      │
   │    Architecture-specific code collects this data and passes it to      │
   │    the lone function which begins execution of the lisp code.          │
   │                                                                        │
   │    During early initialization, lone has no dynamic memory             │
   │    allocation capabilities and so this function statically             │
   │    allocates 64 KiB of memory for the early bootstrapping process.     │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
long lone(int argc, char **argv, char **envp, struct auxiliary *auxv)
{
	#define LONE_BUFFER_SIZE 4096
	#define LONE_MEMORY_SIZE 65536
	static unsigned char memory[LONE_MEMORY_SIZE];
	struct lone_lisp lone;
	struct lone_reader reader;

	lone_lisp_initialize(&lone, memory, sizeof(memory));
	lone_reader_initialize(&lone, &reader, LONE_BUFFER_SIZE, 0);

	struct lone_value *arguments = lone_arguments_to_list(&lone, argc, argv);
	struct lone_value *environment = lone_environment_to_table(&lone, envp);
	struct lone_value *auxiliary_values = lone_auxiliary_vector_to_table(&lone, auxv);

	lone_set_environment(&lone, arguments, environment, auxiliary_values);

	while (!reader.finished) {
		struct lone_value *value = lone_read(&lone, &reader);
		if (!value) { return -1; }

		lone_print(&lone, lone_evaluate(&lone, value), 1);
		linux_write(1, "\n", 1);
	}

	lone_deallocate_all(&lone);

	return 0;
}
