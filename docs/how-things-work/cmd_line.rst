Command Line Processing
=======================

This document describes the command-line parser in
``src/util/pmix_cmd_line.h`` and ``src/util/pmix_cmd_line.c``: how a tool
declares the options it accepts, the syntax a user may write them in, how
the parsed result is read back, and the helpers for options whose *values*
are a small language of their own (``--map-by package:span:pe=2``).

The parser is used by every PMIx tool (``pattrs``, ``pquery``, ``pctrl``,
``palloc``, ...) and by PRRTE, which builds its tools' option tables from
the same macros. The header is installed so that projects layered on PMIx
can use it, which is why generic command-line machinery belongs here rather
than in any one consumer.

For contributors, ``src/util/AGENTS.md`` records the parser's special cases
and the defects that produced them; the unit tests are in
``test/unit/util/util_cmd_line.c``.


Overview
--------

The parser is a wrapper around ``getopt_long(3)``. A tool supplies

* an array of ``struct option`` naming every long option it accepts,
* a ``getopt``-style string of the short options it accepts, and
* the name of its help file,

and ``pmix_cmd_line_parse()`` walks ``argv`` and files every occurrence of
every option into a ``pmix_cli_result_t``. Everything from the first token
that is not an option onward - normally the executable a launcher is to
start, and that executable's own arguments - is returned untouched as the
result's *tail*.

``argv`` itself is never modified: ``getopt`` reorders the array it is
given, so the parser works on a copy.


Declaring the options
---------------------

The option table
^^^^^^^^^^^^^^^^

Options are declared as an array of ``struct option`` terminated by
``PMIX_OPTION_END``. Two macros fill in an entry:

``PMIX_OPTION_DEFINE(name, arg)``
    A long option with no single-character equivalent.

``PMIX_OPTION_SHORT_DEFINE(name, arg, c)``
    A long option that may also be given as ``-c``. The same character
    must appear in the short-option string (below), or ``getopt`` will not
    recognize it.

``arg`` says whether the option takes a value:

``PMIX_ARG_NONE``
    Takes none. Its presence is the whole of what it says, and it is filed
    with no value.

``PMIX_ARG_REQD``
    Requires one, given as the next token or attached with ``=``.

``PMIX_ARG_OPTIONAL``
    May take one, which must then be attached: ``--name=value`` or
    ``-cvalue``. A value given as the next token is *not* taken - it is
    read as the next argument, and usually as the start of the tail. The
    one exception is ``--help``, which the parser treats specially (see
    below).

The option names are ``#define`` s in ``pmix_cmd_line.h`` -
``PMIX_CLI_HELP``, ``PMIX_CLI_PMIXMCA``, ``PMIX_CLI_NAMESPACE`` and so on -
each with a comment giving the argument it takes. Use them rather than
string literals: a misspelled macro is a compile error, a misspelled string
is an option nobody can reach. A project layered on PMIx defines its own
names the same way (PRRTE's are the ``PRTE_CLI_*`` macros in
``src/util/prte_cmd_line.h``).

.. code-block:: c

   static struct option myoptions[] = {
       /* basic options */
       PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_HELP, PMIX_ARG_OPTIONAL, 'h'),
       PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_VERSION, PMIX_ARG_NONE, 'V'),
       PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_VERBOSE, PMIX_ARG_NONE, 'v'),
       PMIX_OPTION_DEFINE(PMIX_CLI_PMIXMCA, PMIX_ARG_REQD),

       /* what this tool does */
       PMIX_OPTION_DEFINE(PMIX_CLI_PID, PMIX_ARG_REQD),
       PMIX_OPTION_DEFINE(PMIX_CLI_NAMESPACE, PMIX_ARG_REQD),
       PMIX_OPTION_DEFINE(PMIX_CLI_SYSTEM_SERVER, PMIX_ARG_NONE),

       PMIX_OPTION_END
   };

Options that take two values
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

A few options are declared ``PMIX_ARG_REQD`` - one value, as far as
``getopt`` knows - and then take a *second* token of the parser's own
accord:

* any option whose name ends in ``mca`` (``--pmixmca``, and PRRTE's
  ``--prtemca``): ``--pmixmca name value`` is filed as the single value
  ``name=value``;
* ``--prepend-env`` and ``--append-env``: ``--prepend-env VAR value`` is
  filed as two values, ``VAR`` then ``value``;
* ``--show-version``, which takes up to two trailing tokens and none at
  all when the next token is another option.

The second token must be present and must not name a registered option;
otherwise the command line is refused with a message naming the option.
Whether a token names an option is decided by asking the option table (a
long option or an abbreviation of one, or a cluster of registered short
options), not by looking for a dash, because a value may legitimately begin
with one (``--pmixmca foo -1``, negative MCA values being real) or contain
one (``0-1``).

The short-option string
^^^^^^^^^^^^^^^^^^^^^^^

The short options are given as a ``getopt(3)`` option string:

``c``
    ``-c`` takes no value.

``c:``
    ``-c`` requires a value: ``-c value`` or ``-cvalue``.

``c::``
    ``-c`` may take a value, which must be attached: ``-cvalue``. Written
    ``-c value``, the value is not taken.

Every character in the string must also be the ``c`` of a
``PMIX_OPTION_SHORT_DEFINE`` entry; a short option with no long equivalent
is refused when it is used. Pass ``NULL`` for a tool with no short options.

Do not prefix the string with ``+`` or ``-`` - the parser prefixes it with
``+`` itself (see "Where the options end", below). The usual string for a
tool whose only short options are the basic ones is:

.. code-block:: c

   static char *myshorts = "h::vV";

Three short options are handled by the parser itself rather than filed:

``-h`` / ``--help``
    With no value, prints the ``usage`` topic of the tool's help file.
    With a value - ``--help topic``, ``--help=topic`` or ``-htopic``, any
    leading dashes stripped - prints that topic, which by convention is
    named after the option it describes, so ``--help pid`` explains
    ``--pid``. ``help``, ``version`` and ``verbose`` are answered from
    PMIx's own ``help-cli.txt``.

``-V`` / ``--version``
    Prints the ``version`` topic of the tool's help file.

``-v`` / ``--verbose``
    Filed with a value that counts the ``v``\ s: ``-vvv`` is ``3``, while
    ``--verbose`` and ``-v`` are each ``1``. Repeated, it is filed once per
    occurrence, and the caller adds them up.

For both help and version the parse ends there and returns
``PMIX_OPERATION_SUCCEEDED``.

Finally, ``-np N`` is accepted as a spelling of ``--np N`` when the tool
registers ``np`` with a short ``n``: ``getopt`` reads ``-np`` as ``-n``
with the value ``p``, and the parser recognizes that and takes the next
token as the count.


Parsing
-------

.. code-block:: c

   int pmix_cmd_line_parse(char **argv, char *shorts,
                           struct option myoptions[],
                           pmix_cmd_line_store_fn_t storefn,
                           pmix_cli_result_t *results,
                           char *helpfile);

``results`` must already be constructed (``PMIX_CONSTRUCT`` or
``PMIX_CLI_RESULT_STATIC_INIT``). It may already hold values the caller
put there; those are kept, and are recorded as having no position on the
command line.

``storefn`` is ``NULL`` for the default, which files each occurrence under
the option's name, appending its value (if any) to that option's list. A
tool may supply its own to file values differently - under another key, or
several - and the positions described below are still recorded correctly,
because they are worked out from what the result holds afterwards rather
than from what the store function says it did.

The return value says what the caller should do next:

``PMIX_SUCCESS``
    The command line was parsed. Carry on.

``PMIX_OPERATION_SUCCEEDED``
    The parse is over and the tool has already said its piece
    (``--help``, ``--version``). Print nothing more and exit
    successfully.

``PMIX_ERR_SILENT``
    The command line was refused and the reason has already been printed.
    Exit without adding a message of your own.

anything else
    A failure nothing has reported yet - say so, for instance with
    ``PMIx_Error_string()``.

.. code-block:: c

   pmix_cli_result_t results;
   int rc;

   PMIX_CONSTRUCT(&results, pmix_cli_result_t);
   rc = pmix_cmd_line_parse(argv, myshorts, myoptions, NULL,
                            &results, "help-mytool.txt");
   if (PMIX_SUCCESS != rc) {
       if (PMIX_ERR_SILENT != rc && PMIX_OPERATION_SUCCEEDED != rc) {
           fprintf(stderr, "%s: command line error (%s)\n",
                   argv[0], PMIx_Error_string(rc));
       }
       exit((PMIX_OPERATION_SUCCEEDED == rc) ? 0 : 1);
   }

The parser keeps its state in ``getopt``'s process-global ``optind``,
``optarg``, ``opterr`` and ``optopt``, which it resets on entry. Two parses
at once corrupt each other, whatever results they are filling: parse once,
at startup, on the tool's main thread.


The command line a user writes
------------------------------

These are the rules a user of any tool built on this parser can rely on.

Long options
^^^^^^^^^^^^

* An option that takes a value may be given it as the next token or
  attached with ``=``: ``--namespace foo`` and ``--namespace=foo`` are the
  same. An option whose value is optional takes it only when attached.

* A long option's name may be abbreviated to any prefix that names only
  that option - ``getopt_long`` accepts ``--names`` for ``--namespace`` in
  a tool with no other option beginning that way. A prefix that fits two
  options is refused.

* Each long option is written with two dashes. ``-np`` is the one
  single-dash spelling of a long option the parser accepts.

Short options
^^^^^^^^^^^^^

* Short options that take no value may be clustered: ``-vvV``.

* A required value may be attached or be the next token; an optional one
  must be attached (``-hpid``).

Values
^^^^^^

* A value may begin with a dash. It is read as a value unless it names a
  registered option - a long option or an abbreviation of one, or a
  cluster of registered short options - so ``--pmixmca foo -1`` works.

* An option given more than once is filed once per occurrence, in the
  order given. Whether a repeat adds to the earlier ones or contradicts
  them is the tool's decision, not the parser's.

Where the options end
^^^^^^^^^^^^^^^^^^^^^

* Options end at the first token that is not one. That token and
  everything after it are the tail - for a launcher, the executable and
  its own arguments, which must not be read as the launcher's. So in
  ``mytool --verbose app --verbose`` the second ``--verbose`` belongs to
  ``app``.

* ``--`` ends the options explicitly; everything after it is the tail,
  even tokens that look like options.

* A lone ``-`` is not an option. It conventionally names standard input,
  and it begins the tail.

* An ``&`` where the tail would begin - the command being put into the
  background - leaves no tail rather than a tail of ``&``.


Reading the result
------------------

A ``pmix_cli_result_t`` holds a list of ``pmix_cli_item_t``, one per option
given, and the tail:

.. code-block:: c

   typedef struct {
       pmix_list_item_t super;
       char *key;       // the option's name, as declared
       char **values;   // every value given, in order; NULL if it takes none
       int *seq;        // command-line position of each value
       int nseq;
       int firstseq;    // position at which the option first appeared
   } pmix_cli_item_t;

   typedef struct {
       pmix_object_t super;
       pmix_list_t instances;   // pmix_cli_item_t's
       char **tail;             // everything after the options, or NULL
       int nseq;
   } pmix_cli_result_t;

The accessors, all keyed by the option's declared name:

``pmix_cmd_line_get_param(results, key)``
    The option's item, or ``NULL`` if it was not given.

``pmix_cmd_line_is_taken(results, key)``
    Whether it was given at all - how an option that takes no value is
    read.

``pmix_cmd_line_get_ninsts(results, key)``
    How many values it was given.

``pmix_cmd_line_get_nth_instance(results, key, n)``
    Its ``n``\ th value, or ``NULL`` if there is none.

``pmix_cmd_line_get_nth_seq(results, key, n)`` and ``pmix_cmd_line_get_first_seq(results, key)``
    Where on the command line its ``n``\ th value, or its first
    appearance, was - a number meaningful only compared with another from
    the same parse, and ``-1`` for a value the caller put there itself.

``pmix_cmd_line_get_ordered(results, &occurrences, &nocc)``
    Every occurrence of every option, sorted by position. The grouped view
    cannot say whether one ``--set-env`` came before or after a
    ``--prepend-env``; this one can. The array points into the result's
    strings and is released with ``free()``.

.. code-block:: c

   pmix_cli_item_t *opt;

   if (NULL != (opt = pmix_cmd_line_get_param(&results, PMIX_CLI_PID))) {
       pid = strtol(opt->values[0], NULL, 10);
   }
   if (pmix_cmd_line_is_taken(&results, PMIX_CLI_SYSTEM_SERVER)) {
       /* ... */
   }
   /* the executable and its arguments */
   app_argv = results.tail;


Options whose values are directives
-----------------------------------

Some options take a value that is a small language of its own. In PRRTE,
``--map-by package:span:pe=2`` is a *directive* (``package``) followed by
*qualifiers* (``span``, ``pe=2``), each of which may carry a value after
an ``=``, and ``--output tag,timestamp`` is a list of directives. The words
come from a fixed vocabulary and may be abbreviated. ``getopt`` has
nothing to say about any of this; these helpers do.

The separators are the consumer's to define, but the convention - which
PRRTE follows - is:

* ``,`` separates directives,
* ``:`` separates a directive from its qualifiers, and one qualifier from
  the next,
* ``=`` separates a word from its value.

So a qualifier written after a ``,`` is not a qualifier at all:
``device=gpu,ndev=2`` is a device named ``gpu,ndev=2``, and the ``ndev``
qualifier is spelled ``device=gpu:ndev=2``.

Matching one word: ``pmix_check_cli_option()``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   bool pmix_check_cli_option(char *input, char *name);
   #define PMIX_CHECK_CLI_OPTION(input, name) ...

True if ``input`` names ``name``: spells it out, or abbreviates it.

* Comparison is case-insensitive.
* Both strings are cut at their first ``=``, so the value never takes part,
  and a name may be declared with its ``=`` (``"pe="``).
* An abbreviation is a leading part of the name: never empty and never
  longer than the name. ``packagefoo`` is not ``package``, and
  ``gpu,ndev=2`` is not ``gpu``.
* A hyphenated name is abbreviated segment by segment: ``sys-serv`` is
  ``system-server``, but ``systemserver`` is not, and a word with more
  segments than the name never matches it.

This answers for one name at a time, so it cannot tell whether an
abbreviation *also* fits some other word the caller accepts - a chain of
``if (PMIX_CHECK_CLI_OPTION(...))`` tests takes the first that fits, and
the order the chain happens to be written in then decides what an
ambiguous word means. Where the input could be one of several words, use
``pmix_cli_match()``.

Matching against a vocabulary: ``pmix_cli_match()``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

A vocabulary is a ``pmix_cli_choice_t`` table ended by
``PMIX_CLI_CHOICE_END``:

.. code-block:: c

   PMIX_CLI_CHOICE(name, tag, value)

``name``
    The word in full. Anything from an ``=`` on is ignored, so ``"pe="``
    can be used as it stands.

``tag``
    What the match reports. Entries that share a tag are spellings of one
    thing (``parseable`` and ``parsable``), and an input matching several
    of them is not ambiguous. Switch on the tag, never on a position in
    the table.

``value``
    ``PMIX_CLI_VALUE_NONE`` - takes no value (``span``);
    ``PMIX_CLI_VALUE_OPTIONAL`` - may take one (``shared`` or
    ``shared=false``); ``PMIX_CLI_VALUE_REQUIRED`` - must have one
    (``pe=2``).

.. code-block:: c

   pmix_cli_match_t pmix_cli_match(const char *input,
                                   const pmix_cli_choice_t *choices,
                                   int *tag);

applies these rules in order:

#. an input that spells a word out in full is that word, whatever else it
   also abbreviates - ``pe`` is ``pe``, not ``pe-list``;
#. otherwise the input must abbreviate exactly one word, or words that all
   share a tag;
#. the matched word's value rule is then applied. An ``=`` with nothing
   after it promises a value and does not give one, so it counts as
   missing wherever a value is allowed.

and returns one of

``PMIX_CLI_MATCH_FOUND``
    ``*tag`` is the word's tag.

``PMIX_CLI_MATCH_NONE``
    Matches no word (including an empty or ``NULL`` input). ``*tag`` is
    ``-1``.

``PMIX_CLI_MATCH_AMBIGUOUS``
    Abbreviates words with different tags. ``*tag`` is ``-1``.

``PMIX_CLI_MATCH_UNEXPECTED_VALUE``
    A value was given to a word that takes none - ``span=false``, which a
    comparison that stops at the ``=`` would have read as ``span``.
    ``*tag`` is the word's tag, so the caller can name it.

``PMIX_CLI_MATCH_MISSING_VALUE``
    A word that needs a value was given none. ``*tag`` is the word's tag.

The value itself is read with ``pmix_cli_qualifier_value()``.

.. code-block:: c

   char *pmix_cli_match_list(const char *input,
                             const pmix_cli_choice_t *choices,
                             char sep);

returns the names of the words ``input`` matches, joined by ``sep`` and
without any ``=`` - the list to show the user after an ambiguous input.
Given a ``NULL`` input it lists every word, which is the list of valid
spellings to show after an input that matched nothing. Returns ``NULL``
if nothing matches; the caller frees the result.

An example: the policy word of a ``--bind-to``-style option, where ``n``
fits both ``none`` and ``numa``:

.. code-block:: c

   enum { BIND_NONE, BIND_CORE, BIND_NUMA, BIND_PACKAGE };

   static const pmix_cli_choice_t binders[] = {
       PMIX_CLI_CHOICE("none", BIND_NONE, PMIX_CLI_VALUE_NONE),
       PMIX_CLI_CHOICE("core", BIND_CORE, PMIX_CLI_VALUE_NONE),
       PMIX_CLI_CHOICE("numa", BIND_NUMA, PMIX_CLI_VALUE_NONE),
       PMIX_CLI_CHOICE("package", BIND_PACKAGE, PMIX_CLI_VALUE_NONE),
       PMIX_CLI_CHOICE("socket", BIND_PACKAGE, PMIX_CLI_VALUE_NONE),  // older spelling
       PMIX_CLI_CHOICE_END
   };

   int tag;
   char *list;

   switch (pmix_cli_match(word, binders, &tag)) {
       case PMIX_CLI_MATCH_FOUND:
           break;
       case PMIX_CLI_MATCH_AMBIGUOUS:
           list = pmix_cli_match_list(word, binders, ',');
           fprintf(stderr, "\"%s\" could be any of: %s\n", word, list);
           free(list);
           return ERROR;
       case PMIX_CLI_MATCH_NONE:
           list = pmix_cli_match_list(NULL, binders, ',');
           fprintf(stderr, "\"%s\" is not one of: %s\n", word, list);
           free(list);
           return ERROR;
       default:
           fprintf(stderr, "\"%s\": wrong value for %s\n",
                   word, binders[tag].name);   // tags here are indices
           return ERROR;
   }

With that table, ``c``, ``co`` and ``core`` are all ``BIND_CORE``;
``n`` is ambiguous; ``nu`` is ``BIND_NUMA``; ``s`` is ``BIND_PACKAGE``;
``cores`` and ``core=2`` are refused.

``pmix_cli_match()`` is available when ``PMIX_CAP_CLI_MATCH`` is defined
in ``pmix_version.h``.

Reading a value: ``pmix_cli_qualifier_value()``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   char *pmix_cli_qualifier_value(char *qual);
   #define PMIX_CLI_QUALIFIER_VALUE(q) ...

returns the text after a word's ``=``, or ``NULL`` if there is none or it
is empty. The result points into ``qual``.

Always read a value this way, never at an offset fixed to the word's full
spelling: the user may have abbreviated it, so ``p=2`` is ``pe=2`` and the
value is not three characters in.

Reading a time: ``pmix_convert_string_to_time()``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: c

   unsigned int pmix_convert_string_to_time(const char *t);
   #define PMIX_CONVERT_TIME(t) ...

converts ``[[[days:]hours:]minutes:]seconds`` to a number of seconds:
``90`` and ``1:30`` are both ninety seconds, ``1:0:0`` an hour. An empty
string, or one with no fields, is zero.

Because a time contains ``:``, an option whose values may be times cannot
also use ``:`` to separate qualifiers - PRRTE's ``--rtos`` has no
qualifiers for exactly that reason.
