/* SPDX-License-Identifier: AGPL-3.0-or-later */

/* ╭─────────────────────────────┨ LONE LISP ┠──────────────────────────────╮
   │                                                                        │
   │                       The standalone Linux Lisp                        │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
#include <linux/types.h>
#include <linux/unistd.h>

typedef __kernel_size_t size_t;
typedef __kernel_ssize_t ssize_t;

/* ╭────────────────────────────────────────────────────────────────────────╮
   │                                                                        │
   │    Definitions for essential Linux system calls used by lone.          │
   │    The exit system call is adorned with compiler annotations           │
   │    for better code generation.                                         │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static void __attribute__((noreturn)) linux_exit(int code)
{
	system_call_1(__NR_exit, code);
	__builtin_unreachable();
}

static ssize_t linux_read(int fd, const void *buffer, size_t count)
{
	return system_call_3(__NR_read, fd, (long) buffer, count);
}

static ssize_t linux_write(int fd, const void *buffer, size_t count)
{
	return system_call_3(__NR_write, fd, (long) buffer, count);
}

/* ╭──────────────────────────┨ LONE LISP TYPES ┠───────────────────────────╮
   │                                                                        │
   │    Lone is designed to work without any dependencies except Linux,     │
   │    so it does not make use of even the system's C library.             │
   │    In order to bootstrap itself in such harsh conditions,              │
   │    it must be given some memory to work with.                          │
   │    The lone_linux structure holds that memory.                         │
   │                                                                        │
   │    Lone implements dynamic data types as a tagged union.               │
   │    Supported types are:                                                │
   │                                                                        │
   │        ◦ Bytes    the binary data and low level string type            │
   │        ◦ List     the linked list and tree type                        │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
struct lone_lisp {
	unsigned char *memory;
	size_t capacity;
	size_t allocated;
};

enum lone_type {
	LONE_BYTES = 0,
	LONE_LIST = 1,
};

struct lone_bytes {
	size_t count;
	unsigned char *pointer;
};

struct lone_list {
	struct lone_value *first;
	struct lone_value *rest;
};

struct lone_value {
	enum lone_type type;
	union {
		struct lone_bytes bytes;
		struct lone_list list;
	};
};

/* ╭────────────────────┨ LONE LISP MEMORY ALLOCATION ┠─────────────────────╮
   │                                                                        │
   │    Lone will allocate from its internal memory at first.               │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
static void *lone_allocate(struct lone_lisp *lone, size_t size)
{
	if (lone->allocated + size > lone->capacity)
		linux_exit(-1);
	void *allocation = lone->memory + lone->allocated;
	lone->allocated += size;
	return allocation;
}

static struct lone_value *lone_value_create(struct lone_lisp *lone)
{
	return lone_allocate(lone, sizeof(struct lone_value));
}

static struct lone_value *lone_bytes_create(struct lone_lisp *lone, unsigned char *pointer, size_t count)
{
	struct lone_value *value = lone_value_create(lone);
	value->type = LONE_BYTES;
	value->bytes.count = count;
	value->bytes.pointer = pointer;
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

static struct lone_value *lone_list_set(struct lone_value *list, struct lone_value *value)
{
	return list->list.first = value;
}

static struct lone_value *lone_list_append(struct lone_value *list, struct lone_value *rest)
{
	return list->list.rest = rest;
}

static struct lone_value *lone_list_last(struct lone_value *list)
{
	while (list->list.rest != 0) {
		list = list->list.rest;
	}
	return list;
}

/* ╭──────────────────────────┨ LONE LISP LEXER ┠───────────────────────────╮
   │                                                                        │
   │    The lexer or tokenizer transforms a linear stream of characters     │
   │    into a linear stream of tokens suitable for parser consumption.     │
   │    This gets rid of insignificant whitespace and reduces the size      │
   │    of the parser's input significantly.                                │
   │                                                                        │
   │    The lone lisp lexer receives as input a single lone bytes value     │
   │    containing the full source code to be processed and it outputs      │
   │    a lone list of each lisp token found in the input. For example:     │
   │                                                                        │
   │        lexer ← lone_bytes = [ ("abc" ("zxc") ]                         │
   │        lexer → lone_list  = { [(] → ["abc"] → [(] → ["zxc"] → [)] }    │
   │                                                                        │
   │    Note that parentheses are not matched at this stage.                │
   │    The lexical analysis algorithm can be broken down as follows:       │
   │                                                                        │
   │        ◦ Skip all whitespace until it finds something                  │
   │        ◦ If found sign before digits take note and process the digits  │
   │        ◦ If found digit then look for more digits and tokenize all     │
   │        ◦ If found " then find the next " and tokenize it all           │
   │        ◦ If found ( or ) just tokenize them as is                      │
   │        ◦ Tokenize everything else unmodified as a symbol               │
   │                                                                        │
   ╰────────────────────────────────────────────────────────────────────────╯ */
struct lone_lexer {
	struct lone_bytes input;
	size_t position;
};

static unsigned char *lone_lexer_peek_k(struct lone_lexer *lexer, size_t k)
{
	if (lexer->position + k > lexer->input.count)
		return 0;
	return lexer->input.pointer + lexer->position + k;
}

static unsigned char *lone_lexer_peek(struct lone_lexer *lexer)
{
	return lone_lexer_peek_k(lexer, 0);
}

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

static ssize_t lone_lexer_find_byte(char byte, unsigned char *bytes, size_t size)
{
	size_t i = 0;

	do {
		if (i >= size) { return -1; }
	} while (!lone_lexer_match_byte(bytes[i++], byte));

	return i;
}

static struct lone_value *lone_lex(struct lone_lisp *lone, struct lone_value *value)
{
	struct lone_value *first = lone_list_create_nil(lone), *current = first;
	unsigned char *input = value->bytes.pointer;
	size_t size = value->bytes.count;

	for (size_t i = 0; i < size; ++i) {
		if (lone_lexer_match_byte(input[i], ' ')) {
			continue;
		} else {
			size_t start = i, remaining = size - start, end = 0;
			unsigned char *position = input + start;

			int is_signed_number =
				   ((*position == '+') || (*position == '-'))
				&& (start < size - 1) // is this the last char?
				&& lone_lexer_match_byte(position[1], '1');
			if (is_signed_number) {
				// make sure we tokenize as a number
				++position;
			}

			switch (*position) {
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				while ((end + 1) < remaining && lone_lexer_match_byte(position[++end], '1'));
				if (!lone_lexer_match_byte(position[end], ' ')) { goto lex_failed; }
				// include the sign in the token if present
				if (is_signed_number) { --position; ++end; }
				lone_list_set(current, lone_bytes_create(lone, position, end));
				break;
			case '"':
				end = lone_lexer_find_byte('"', position + 1, remaining - 1);
				if (end < 0) { goto lex_failed; }
				lone_list_set(current, lone_bytes_create(lone, position, end + 1));
				break;
			case '(':
			case ')':
				lone_list_set(current, lone_bytes_create(lone, position, 1));
				break;
			default:
				while ((end + 1) < remaining && !lone_lexer_match_byte(position[++end], ' '));
				lone_list_set(current, lone_bytes_create(lone, position, end));
			}
			current = lone_list_append(current, lone_list_create_nil(lone));
			i += end;
		}
	}

	return first;

lex_failed:
	linux_exit(-1);
}

static struct lone_value *lone_parse(struct lone_lisp *lone, struct lone_value *value)
{
	return lone_lex(lone, value);
}

static struct lone_value *lone_read_all_input(struct lone_lisp *lone, int fd)
{
	#define LONE_BUFFER_SIZE 4096
	unsigned char *input = lone_allocate(lone, LONE_BUFFER_SIZE);
	size_t bytes_read = 0, total_read = 0;

	while (1) {
		bytes_read = linux_read(fd, input + total_read, LONE_BUFFER_SIZE);

		if (bytes_read < 0) {
			linux_exit(-1);
		}

		total_read += bytes_read;

		if (bytes_read == LONE_BUFFER_SIZE) {
			lone_allocate(lone, LONE_BUFFER_SIZE);
		} else {
			break;
		}
	}

	return lone_bytes_create(lone, input, total_read);
}

static struct lone_value *lone_read(struct lone_lisp *lone, int fd)
{
	return lone_parse(lone, lone_read_all_input(lone, fd));
}

static struct lone_value *lone_evaluate(struct lone_lisp *lone, struct lone_value *value)
{
	switch (value->type) {
	case LONE_BYTES:
	case LONE_LIST:
		return value;
		break;
	default:
		linux_exit(-1);
	}
}

static void lone_print(struct lone_lisp *lone, struct lone_value *value, int fd)
{
	if (value == 0)
		return;

	switch (value->type) {
	case LONE_LIST:
		lone_print(lone, value->list.first, fd);
		lone_print(lone, value->list.rest, fd);
		break;
	case LONE_BYTES:
		linux_write(fd, value->bytes.pointer, value->bytes.count);
		break;
	default:
		linux_exit(-1);
	}
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
#if __BITS_PER_LONG == 64
typedef __u64 auxiliary_value;
#elif __BITS_PER_LONG == 32
typedef __u32 auxiliary_value;
#else
#error "Unsupported architecture"
#endif

struct auxiliary {
	auxiliary_value type;
	auxiliary_value value;
};

long lone(int count, char **arguments, char **environment, struct auxiliary *values)
{
	#define LONE_MEMORY_SIZE 65536
	static unsigned char memory[LONE_MEMORY_SIZE];
	struct lone_lisp lone = { memory, sizeof(memory), 0 };

	lone_print(&lone, lone_evaluate(&lone, lone_read(&lone, 0)), 1);

	return 0;
}
