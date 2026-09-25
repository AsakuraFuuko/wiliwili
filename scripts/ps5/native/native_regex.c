/*
 * wiliwili PS5 native application - POSIX regex subset.
 * Copyright (C) 2026 wiliwili contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The clean-room runtime ships no regular-expression engine, and the port
 * libraries that need one (mpv's filter parsing) only use ordinary extended
 * expressions. This is a compact backtracking engine over byte strings that
 * implements the extended syntax below and reports REG_BADPAT for anything it
 * does not support (back references, collating elements, wide-character and
 * locale-dependent classes). Character classes are ASCII; the process has no
 * locale database, so this matches the C locale.
 *
 * Supported: literals, '.', '^', '$', '[...]' with ranges/negation and the
 * ASCII '[[:class:]]' names, '*', '+', '?', '{m,n}', grouping and '|', with
 * backslash escapes. Both REG_EXTENDED and the basic dialect's escaped
 * operators are accepted.
 */

#include <regex.h>
#include <stdlib.h>
#include <string.h>

enum node_kind {
  NODE_EMPTY,
  NODE_CHAR,
  NODE_ANY,
  NODE_CLASS,
  NODE_BOL,
  NODE_EOL,
  NODE_GROUP,
  NODE_REPEAT,
  NODE_ALT,
};

struct node {
  enum node_kind kind;
  unsigned char ch;
  unsigned char mask[32];
  int min;
  int max; /* -1 = unbounded */
  int first; /* index of first child */
  int count; /* number of children */
  int next;  /* index of the following node in the same sequence */
};

struct program {
  struct node *nodes;
  size_t count;
  size_t capacity;
  int error;
};

struct compiled {
  struct program program;
  int root;
  int flags;
};

struct parser {
  const char *pattern;
  size_t position;
  struct program *program;
};

static const int k_max_nodes = 8192;

static int add_node(struct program *program, enum node_kind kind) {
  if (program->count >= program->capacity) {
    size_t grown = program->capacity == 0 ? 64 : program->capacity * 2;
    if (grown > (size_t)k_max_nodes) {
      program->error = REG_ESPACE;
      return -1;
    }
    struct node *nodes = realloc(program->nodes, grown * sizeof(*nodes));
    if (nodes == NULL) {
      program->error = REG_ESPACE;
      return -1;
    }
    program->nodes = nodes;
    program->capacity = grown;
  }
  struct node *node = &program->nodes[program->count];
  memset(node, 0, sizeof(*node));
  node->kind = kind;
  node->next = -1;
  node->first = -1;
  return (int)program->count++;
}

static void class_set(struct node *node, unsigned char c) {
  node->mask[c >> 3] |= (unsigned char)(1u << (c & 7));
}

static int class_test(const struct node *node, unsigned char c) {
  return (node->mask[c >> 3] >> (c & 7)) & 1u;
}

static void class_add_named(struct node *node, const char *name, size_t length) {
  for (int c = 0; c < 128; ++c) {
    int member = 0;
    if (length == 5 && strncmp(name, "alpha", 5) == 0)
      member = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    else if (length == 5 && strncmp(name, "digit", 5) == 0)
      member = c >= '0' && c <= '9';
    else if (length == 5 && strncmp(name, "alnum", 5) == 0)
      member = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    else if (length == 5 && strncmp(name, "space", 5) == 0)
      member = c == ' ' || (c >= '\t' && c <= '\r');
    else if (length == 5 && strncmp(name, "upper", 5) == 0)
      member = c >= 'A' && c <= 'Z';
    else if (length == 5 && strncmp(name, "lower", 5) == 0)
      member = c >= 'a' && c <= 'z';
    else if (length == 5 && strncmp(name, "print", 5) == 0)
      member = c >= 0x20 && c < 0x7f;
    else if (length == 5 && strncmp(name, "graph", 5) == 0)
      member = c > 0x20 && c < 0x7f;
    else if (length == 4 && strncmp(name, "punct", 4) == 0)
      member = c > 0x20 && c < 0x7f && !((c >= 'a' && c <= 'z') ||
                                         (c >= 'A' && c <= 'Z') ||
                                         (c >= '0' && c <= '9'));
    else if (length == 5 && strncmp(name, "cntrl", 5) == 0)
      member = c < 0x20 || c == 0x7f;
    else if (length == 5 && strncmp(name, "xdigit", 5) == 0)
      member = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    if (member)
      class_set(node, (unsigned char)c);
  }
}

static int parse_sequence(struct parser *parser, int extended);

static unsigned char parse_escape(struct parser *parser, int *is_class) {
  *is_class = 0;
  if (parser->pattern[parser->position] != '\\')
    return (unsigned char)parser->pattern[parser->position++];
  parser->position++;
  char escaped = parser->pattern[parser->position];
  if (escaped == '\0')
    return '\\';
  parser->position++;
  switch (escaped) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return '\0';
    default: return (unsigned char)escaped;
  }
}

static int parse_class(struct parser *parser, int *negated) {
  /* parser->position points just past the opening bracket. */
  int node = add_node(parser->program, NODE_CLASS);
  if (node < 0)
    return -1;
  struct node *out = &parser->program->nodes[node];

  if (parser->pattern[parser->position] == '^') {
    *negated = 1;
    parser->position++;
  } else {
    *negated = 0;
  }

  int first = 1;
  while (parser->pattern[parser->position] != '\0') {
    char c = parser->pattern[parser->position];
    if (c == ']' && !first)
      break;
    first = 0;

    if (c == '[' && parser->pattern[parser->position + 1] == ':') {
      const char *closing = strstr(parser->pattern + parser->position + 2, ":]");
      if (closing != NULL) {
        class_add_named(out, parser->pattern + parser->position + 2,
                        (size_t)(closing - (parser->pattern + parser->position + 2)));
        parser->position = (size_t)(closing - parser->pattern) + 2;
        continue;
      }
    }

    unsigned char low;
    int is_class = 0;
    if (c == '\\')
      low = parse_escape(parser, &is_class);
    else {
      low = (unsigned char)c;
      parser->position++;
    }

    if (parser->pattern[parser->position] == '-' &&
        parser->pattern[parser->position + 1] != ']' &&
        parser->pattern[parser->position + 1] != '\0') {
      parser->position++;
      unsigned char high;
      if (parser->pattern[parser->position] == '\\')
        high = parse_escape(parser, &is_class);
      else
        high = (unsigned char)parser->pattern[parser->position++];
      for (unsigned value = low; value <= high; ++value)
        class_set(out, (unsigned char)value);
    } else {
      class_set(out, low);
    }
  }

  if (parser->pattern[parser->position] != ']')
    return -2;
  parser->position++;
  return node;
}

/* Parses one quantified atom and returns its node index. */
static int parse_atom(struct parser *parser, int extended) {
  char c = parser->pattern[parser->position];
  int node;
  int negated = 0;

  switch (c) {
    case '(':
      parser->position++;
      node = parse_sequence(parser, extended);
      if (node < 0)
        return node;
      if (parser->pattern[parser->position] != ')')
        return -3;
      parser->position++;
      break;
    case '[':
      parser->position++;
      node = parse_class(parser, &negated);
      if (node < 0)
        return node;
      if (negated) {
        struct node *class_node = &parser->program->nodes[node];
        for (size_t i = 0; i < sizeof(class_node->mask); ++i)
          class_node->mask[i] = (unsigned char)~class_node->mask[i];
      }
      break;
    case '.':
      parser->position++;
      node = add_node(parser->program, NODE_ANY);
      break;
    case '^':
      parser->position++;
      node = add_node(parser->program, NODE_BOL);
      break;
    case '$':
      parser->position++;
      node = add_node(parser->program, NODE_EOL);
      break;
    case '\\': {
      int is_class = 0;
      unsigned char value = parse_escape(parser, &is_class);
      node = add_node(parser->program, NODE_CHAR);
      if (node < 0)
        return -1;
      parser->program->nodes[node].ch = value;
      break;
    }
    case '\0':
      return -4;
    default:
      parser->position++;
      node = add_node(parser->program, NODE_CHAR);
      if (node < 0)
        return -1;
      parser->program->nodes[node].ch = (unsigned char)c;
      break;
  }
  return node;
}

static int parse_repeat(struct parser *parser, int atom, int extended) {
  char c = parser->pattern[parser->position];
  int min = -1;
  int max = -1;

  if (c == '*') {
    min = 0;
    max = -1;
    parser->position++;
  } else if (c == '+') {
    min = 1;
    max = -1;
    parser->position++;
  } else if (c == '?') {
    min = 0;
    max = 1;
    parser->position++;
  } else if (c == '{') {
    size_t saved = parser->position;
    parser->position++;
    int low = 0, high = -1, digits = 0;
    while (parser->pattern[parser->position] >= '0' &&
           parser->pattern[parser->position] <= '9') {
      low = low * 10 + (parser->pattern[parser->position] - '0');
      parser->position++;
      digits++;
    }
    if (parser->pattern[parser->position] == ',') {
      parser->position++;
      if (parser->pattern[parser->position] >= '0' &&
          parser->pattern[parser->position] <= '9') {
        high = 0;
        while (parser->pattern[parser->position] >= '0' &&
               parser->pattern[parser->position] <= '9') {
          high = high * 10 + (parser->pattern[parser->position] - '0');
          parser->position++;
        }
      }
    } else {
      high = low;
    }
    const int terminated = parser->pattern[parser->position] == '}';
    if (terminated && digits > 0 && (high < 0 || low <= high)) {
      parser->position++;
      min = low;
      max = high;
    } else {
      parser->position = saved;
      return atom;
    }
  } else {
    return atom;
  }

  if ((parser->pattern[parser->position] == '*' ||
       parser->pattern[parser->position] == '+' ||
       parser->pattern[parser->position] == '?') &&
      extended)
    return -5;

  int node = add_node(parser->program, NODE_REPEAT);
  if (node < 0)
    return -1;
  parser->program->nodes[node].first = atom;
  parser->program->nodes[node].count = 1;
  parser->program->nodes[node].min = min;
  parser->program->nodes[node].max = max;
  return node;
}

static int parse_branch(struct parser *parser, int extended) {
  int head = -1;
  int tail = -1;

  while (1) {
    char c = parser->pattern[parser->position];
    if (c == '\0' || c == '|' || c == ')')
      break;
    int atom = parse_atom(parser, extended);
    if (atom < 0)
      return atom;
    atom = parse_repeat(parser, atom, extended);
    if (atom < 0)
      return atom;
    if (head < 0) {
      head = atom;
    } else {
      parser->program->nodes[tail].next = atom;
    }
    tail = atom;
  }

  if (head < 0) {
    head = add_node(parser->program, NODE_EMPTY);
    if (head < 0)
      return -1;
  }
  return head;
}

static int parse_sequence(struct parser *parser, int extended) {
  int head = parse_branch(parser, extended);
  if (head < 0)
    return head;
  if (parser->pattern[parser->position] != '|')
    return head;

  int alt = add_node(parser->program, NODE_ALT);
  if (alt < 0)
    return -1;
  int first = add_node(parser->program, NODE_EMPTY);
  if (first < 0)
    return -1;
  parser->program->nodes[first].first = head;
  parser->program->nodes[first].count = 1;

  parser->program->nodes[alt].first = first;
  parser->program->nodes[alt].count = 1;

  int previous_wrapper = first;
  while (parser->pattern[parser->position] == '|') {
    parser->position++;
    int branch = parse_branch(parser, extended);
    if (branch < 0)
      return branch;
    int wrapper = add_node(parser->program, NODE_EMPTY);
    if (wrapper < 0)
      return -1;
    parser->program->nodes[wrapper].first = branch;
    parser->program->nodes[wrapper].count = 1;
    /* Branches are chained: each wrapper's next node is the following one. */
    parser->program->nodes[previous_wrapper].next = wrapper;
    previous_wrapper = wrapper;
    parser->program->nodes[alt].count++;
  }
  return alt;
}

int regcomp(regex_t *regex, const char *pattern, int flags) {
  if (regex == NULL || pattern == NULL)
    return REG_BADPAT;

  struct program program;
  memset(&program, 0, sizeof(program));
  struct parser parser = {pattern, 0, &program};
  int extended = (flags & REG_EXTENDED) != 0;
  int result = parse_sequence(&parser, extended);
  if (result < 0 || program.error != 0 || parser.pattern[parser.position] != '\0') {
    free(program.nodes);
    int code = program.error != 0 ? program.error : REG_BADPAT;
    return code;
  }

  struct compiled *compiled = calloc(1, sizeof(*compiled));
  if (compiled == NULL) {
    free(program.nodes);
    return REG_ESPACE;
  }
  compiled->program = program;
  compiled->root = result;
  compiled->flags = flags;

  regex->re_magic = 0x57494c49;
  regex->re_nsub = program.count;
  regex->re_endp = pattern;
  regex->re_g = (struct re_guts *)compiled;
  return 0;
}

struct matcher {
  const char *subject;
  size_t length;
  struct node *nodes;
  int steps;
};

static int match_node(struct matcher *matcher, int index, size_t position,
                      size_t *out);

static int match_sequence(struct matcher *matcher, int index, size_t position,
                          size_t *out) {
  if (index < 0) {
    *out = position;
    return 1;
  }
  if (++matcher->steps > 2000000)
    return 0;
  struct node *node = &matcher->nodes[index];
  switch (node->kind) {
    case NODE_EMPTY:
      return match_sequence(matcher, node->next, position, out);
    case NODE_GROUP:
      return match_node(matcher, node->first, position, &position)
                 ? match_sequence(matcher, node->next, position, out)
                 : 0;
    default:
      return match_node(matcher, index, position, out);
  }
}

static int match_repeat(struct matcher *matcher, struct node *node,
                        size_t position, size_t *out) {
  int minimum = node->min < 0 ? 0 : node->min;
  int maximum = node->max;
  size_t current = position;

  for (int i = 0; i < minimum; ++i) {
    size_t next;
    if (!match_node(matcher, node->first, current, &next))
      return 0;
    if (next == current && maximum < 0)
      break;
    current = next;
  }

  if (maximum < 0) {
    /* Unbounded: try every longer match, longest first. */
    size_t positions[64];
    size_t count = 0;
    size_t cursor = current;
    positions[count++] = cursor;
    while (count < sizeof(positions) / sizeof(positions[0])) {
      size_t next;
      if (!match_node(matcher, node->first, cursor, &next) || next == cursor)
        break;
      cursor = next;
      positions[count++] = cursor;
    }
    for (size_t i = count; i-- > 0;) {
      if (match_sequence(matcher, node->next, positions[i], out))
        return 1;
    }
    return 0;
  }

  for (int i = minimum; i <= maximum; ++i) {
    if (i > minimum) {
      size_t next;
      if (!match_node(matcher, node->first, current, &next) || next == current)
        break;
      current = next;
    }
    if (match_sequence(matcher, node->next, current, out))
      return 1;
  }
  return 0;
}

static int match_node(struct matcher *matcher, int index, size_t position,
                      size_t *out) {
  if (++matcher->steps > 2000000)
    return 0;
  struct node *node = &matcher->nodes[index];

  switch (node->kind) {
    case NODE_CHAR:
      if (position < matcher->length &&
          (unsigned char)matcher->subject[position] == node->ch)
        return match_sequence(matcher, node->next, position + 1, out);
      return 0;
    case NODE_ANY:
      if (position < matcher->length && matcher->subject[position] != '\n')
        return match_sequence(matcher, node->next, position + 1, out);
      return 0;
    case NODE_CLASS:
      if (position < matcher->length &&
          class_test(node, (unsigned char)matcher->subject[position]))
        return match_sequence(matcher, node->next, position + 1, out);
      return 0;
    case NODE_BOL:
      if (position == 0 ||
          matcher->subject[position - 1] == '\n')
        return match_sequence(matcher, node->next, position, out);
      return 0;
    case NODE_EOL:
      if (position == matcher->length || matcher->subject[position] == '\n')
        return match_sequence(matcher, node->next, position, out);
      return 0;
    case NODE_REPEAT:
      return match_repeat(matcher, node, position, out);
    case NODE_ALT: {
      for (int child = node->first; child >= 0;
           child = matcher->nodes[child].next) {
        struct node *wrapper = &matcher->nodes[child];
        size_t end;
        if (match_node(matcher, wrapper->first, position, &end) &&
            match_sequence(matcher, node->next, end, out))
          return 1;
      }
      return 0;
    }
    case NODE_GROUP:
      return match_node(matcher, node->first, position, out);
    case NODE_EMPTY:
      return match_sequence(matcher, node->next, position, out);
  }
  return 0;
}

int regexec(const regex_t *regex, const char *string, size_t nmatch,
            regmatch_t *matches, int flags) {
  (void)flags;
  if (regex == NULL || regex->re_g == NULL || string == NULL)
    return REG_NOMATCH;

  struct compiled *compiled = (struct compiled *)regex->re_g;
  struct matcher matcher = {string, strlen(string), compiled->program.nodes, 0};

  for (size_t start = 0; start <= matcher.length; ++start) {
    matcher.steps = 0;
    size_t end;
    if (match_node(&matcher, compiled->root, start, &end)) {
      if (nmatch > 0) {
        matches[0].rm_so = (regoff_t)start;
        matches[0].rm_eo = (regoff_t)end;
        for (size_t i = 1; i < nmatch; ++i) {
          matches[i].rm_so = -1;
          matches[i].rm_eo = -1;
        }
      }
      return 0;
    }
  }
  return REG_NOMATCH;
}

size_t regerror(int code, const regex_t *regex, char *buffer, size_t size) {
  (void)regex;
  const char *message;
  switch (code) {
    case 0: message = "success"; break;
    case REG_NOMATCH: message = "no match"; break;
    case REG_BADPAT: message = "invalid regular expression"; break;
    case REG_ESPACE: message = "regular expression too large"; break;
    default: message = "regular expression error"; break;
  }
  size_t length = strlen(message) + 1;
  if (buffer != NULL && size > 0) {
    size_t copy = length < size ? length : size - 1;
    memcpy(buffer, message, copy);
    buffer[copy] = '\0';
  }
  return length;
}

void regfree(regex_t *regex) {
  if (regex == NULL || regex->re_g == NULL)
    return;
  struct compiled *compiled = (struct compiled *)regex->re_g;
  free(compiled->program.nodes);
  free(compiled);
  regex->re_g = NULL;
  regex->re_nsub = 0;
}
