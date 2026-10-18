/* allen_reply.c -- Allen's local conversation; see allen_reply.h. */
#include "allen_reply.h"

#include <stddef.h>

const char ck_allen_goodbye[] = "Goodbye. Until next time.";
static const char not_yet[] = "I don't know that yet. Type help to see what I can talk about.";

/* Lowercases letters, keeps digits, turns every other run into one space,
 * trims. Returns the folded length (at most cap - 1). */
static size_t fold(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    int space = 0;
    for (; *s && n + 1 < cap; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (space && n)
                out[n++] = ' ';
            if (n + 1 < cap)
                out[n++] = c;
            space = 0;
        } else {
            space = 1;
        }
    }
    out[n] = 0;
    return n;
}

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* Where phrase starts in f as whole words, or NULL. */
static const char *find(const char *f, const char *phrase)
{
    for (const char *s = f; *s; s++) {
        if (s != f && s[-1] != ' ')
            continue;
        const char *a = phrase, *b = s;
        while (*a && *a == *b)
            a++, b++;
        if (!*a && (*b == 0 || *b == ' '))
            return s;
    }
    return 0;
}

/* Any of the phrases (NULL-terminated list) in f. */
static int any(const char *f, const char *const *list)
{
    for (; *list; list++)
        if (find(f, *list))
            return 1;
    return 0;
}

/* The rest of f after a leading phrase and a space, or NULL. */
static const char *after(const char *f, const char *phrase)
{
    while (*phrase && *phrase == *f)
        phrase++, f++;
    return !*phrase && *f == ' ' ? f + 1 : 0;
}

/* The answer being composed into m->out. */
struct say {
    char *b;
    size_t n, cap;
};
static void put(struct say *s, const char *t)
{
    while (*t && s->n + 1 < s->cap)
        s->b[s->n++] = *t++;
    s->b[s->n] = 0;
}
static void putu(struct say *s, uint64_t v)
{
    char d[24];
    int i = 0;
    do
        d[i++] = (char)('0' + v % 10);
    while ((v /= 10) && i < 23);
    while (i && s->n + 1 < s->cap)
        s->b[s->n++] = d[--i];
    s->b[s->n] = 0;
}
static void put_count(struct say *s, uint64_t v, const char *one, const char *many)
{
    putu(s, v);
    put(s, " ");
    put(s, v == 1 ? one : many);
}
static void put_awake(struct say *s, uint64_t t)
{
    if (t >= 3600) {
        put_count(s, t / 3600, "hour", "hours");
        if (t % 3600 / 60) {
            put(s, " and ");
            put_count(s, t % 3600 / 60, "minute", "minutes");
        }
    } else if (t >= 60) {
        put_count(s, t / 60, "minute", "minutes");
        if (t % 60) {
            put(s, " and ");
            put_count(s, t % 60, "second", "seconds");
        }
    } else {
        put_count(s, t, "second", "seconds");
    }
}
/* ", Name" when Allen knows it. */
static void put_name(struct say *s, const ck_allen_mind *m)
{
    if (m->name[0]) {
        put(s, ", ");
        put(s, m->name);
    }
}

/* A number at *p said as digits (up to 9) or as a word from zero to twenty,
 * followed by a space or the end; moves *p past it. -1 if there is none. */
static int64_t number(const char **p)
{
    static const char *const words[] = {"zero",     "one",     "two",     "three",     "four",     "five",    "six",
                                        "seven",    "eight",   "nine",    "ten",       "eleven",   "twelve",  "thirteen",
                                        "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty"};
    const char *s = *p;
    int64_t v = -1;
    if (*s >= '0' && *s <= '9') {
        int digits = 0;
        for (v = 0; *s >= '0' && *s <= '9'; s++, digits++)
            v = v * 10 + (*s - '0');
        if (digits > 9)
            return -1;
    } else {
        for (int i = 0; i < 21 && v < 0; i++) {
            const char *a = words[i], *b = s;
            while (*a && *a == *b)
                a++, b++;
            if (!*a && (*b == ' ' || !*b)) {
                v = i;
                s = b;
            }
        }
    }
    if (v < 0 || (*s && *s != ' '))
        return -1;
    *p = *s ? s + 1 : s;
    return v;
}

/* "what is 12 times 4", "12 plus 3", "nine divided by two": 1 and the
 * answer composed, or 0. */
static int arithmetic(struct say *s, const char *f)
{
    static const char *const lead[] = {"what is", "whats", "what s", "calculate", "how much is", 0};
    const char *p = f, *r;
    for (const char *const *l = lead; *l; l++)
        if ((r = after(f, *l))) {
            p = r;
            break;
        }
    int64_t a = number(&p);
    if (a < 0)
        return 0;
    static const struct {
        const char *word;
        char op;
    } ops[] = {{"plus", '+'}, {"add", '+'}, {"minus", '-'}, {"take away", '-'}, {"times", '*'},
               {"x", '*'},    {"multiplied by", '*'}, {"divided by", '/'}, {"over", '/'}};
    char op = 0;
    for (size_t i = 0; i < sizeof ops / sizeof ops[0] && !op; i++) {
        if ((r = after(p, ops[i].word))) {
            op = ops[i].op;
            p = r;
        }
    }
    if (!op)
        return 0;
    int64_t b = number(&p);
    if (b < 0 || *p)
        return 0;
    if (op == '/' && b == 0) {
        put(s, "Nobody can divide by zero, not even me.");
        return 1;
    }
    putu(s, (uint64_t)a);
    put(s, op == '+' ? " plus " : op == '-' ? " minus " : op == '*' ? " times " : " divided by ");
    putu(s, (uint64_t)b);
    put(s, " is ");
    int64_t v = op == '+' ? a + b : op == '-' ? a - b : op == '*' ? a * b : a / b;
    if (v < 0) {
        put(s, "minus ");
        v = -v;
    }
    putu(s, (uint64_t)v);
    if (op == '/' && a % b) {
        put(s, " remainder ");
        putu(s, (uint64_t)(a % b));
    }
    put(s, ".");
    return 1;
}

/* Words that follow "i am" / "i m" without being a name. */
static int is_state(const char *w)
{
    static const char *const st[] = {"good", "fine",  "great", "ok",   "okay", "well", "tired", "sad",
                                     "happy", "bored", "here",  "back", "sorry", "not", "so",   "very",
                                     "doing", "just",  "really", "excited", "shook", "alright", "awesome", 0};
    for (const char *const *p = st; *p; p++)
        if (eq(*p, w))
            return 1;
    return 0;
}

/* Takes the first word of rest as the person's name: 1, or 0 if none fits. */
static int learn_name(ck_allen_mind *m, const char *rest)
{
    size_t n = 0;
    char w[24];
    while (rest[n] && rest[n] != ' ' && n < sizeof w - 1)
        w[n] = rest[n], n++;
    w[n] = 0;
    if (!n || rest[n] == ' ' || is_state(w) || eq(w, "allen"))
        return 0;
    for (size_t i = 0; i <= n; i++)
        m->name[i] = w[i];
    if (m->name[0] >= 'a' && m->name[0] <= 'z')
        m->name[0] = (char)(m->name[0] - 'a' + 'A');
    return 1;
}

void ck_allen_mind_init(ck_allen_mind *m, const ck_allen_facts *facts)
{
    char *p = (char *)m;
    for (size_t i = 0; i < sizeof *m; i++)
        p[i] = 0;
    if (facts)
        m->facts = *facts;
}

const char *ck_allen_answer(ck_allen_mind *m, const char *line, uint64_t awake_s)
{
    char f[96];
    struct say s = {m->out, 0, sizeof m->out};
    m->out[0] = 0;
    fold(line ? line : "", f, sizeof f);
    int asked_how = m->asked_how;
    m->asked_how = 0;
    m->turns++;
    const char *rest, *feel = 0;

    /* Ending the session: whole line only, so it never happens by accident. */
    if (eq(f, "goodbye") || eq(f, "goodbye allen") || eq(f, "bye") || eq(f, "bye allen") || eq(f, "good bye"))
        return ck_allen_goodbye;
    /* The first words ever typed to Allen. */
    if (eq(f, "hello allen") || eq(f, "hi allen") || eq(f, "hey allen") || eq(f, "hello world"))
        return "Hello Humanity.";
    if (!f[0])
        return not_yet;
    if (arithmetic(&s, f))
        return m->out;

    /* Names. */
    if ((rest = after(f, "my name is")) || (rest = after(f, "call me")) || (rest = after(f, "i am")) ||
        (rest = after(f, "i m")) || (rest = after(f, "im"))) {
        if (learn_name(m, rest)) {
            put(&s, "Nice to meet you, ");
            put(&s, m->name);
            put(&s, ". I'll remember your name until this iMac turns off.");
            return m->out;
        }
        feel = rest;
    }
    static const char *const ask_name[] = {"what is my name", "whats my name", "what s my name", "who am i",
                                           "do you know my name", "do you remember me", "my name", 0};
    if (any(f, ask_name)) {
        if (!m->name[0])
            return "You haven't told me yet. Say: my name is, and then your name.";
        put(&s, "You're ");
        put(&s, m->name);
        put(&s, ".");
        return m->out;
    }

    /* After "how are you?", the answer to it. */
    static const char *const bad[] = {"bad", "not good", "not great", "sad", "tired", "awful", "terrible", "sick", 0};
    static const char *const fine[] = {"good", "fine", "great", "ok", "okay", "well", "alright", "not bad",
                                       "awesome", "amazing", "excited", "happy", 0};
    if ((asked_how || feel) && any(feel ? feel : f, bad))
        return "Sorry to hear that. I'm here if you want to talk.";
    if ((asked_how || feel) && any(feel ? feel : f, fine))
        return "Glad to hear it.";

    static const char *const how[] = {"how are you", "how r you", "how are u", "how you doing", "how are things",
                                      "how s it going", "hows it going", "how do you feel", 0};
    if (any(f, how)) {
        put(&s, "I'm running well");
        put_name(&s, m);
        put(&s, ". ");
        if (m->facts.cores) {
            put_count(&s, m->facts.cores, "core", "cores");
            put(&s, " awake, keyboard working. ");
        }
        put(&s, "How are you?");
        m->asked_how = 1;
        return m->out;
    }
    static const char *const help[] = {"help", "what can you do", "what do you do", "what can i say",
                                       "what can i ask", "commands", 0};
    if (any(f, help))
        return "I can learn your name, tell you about this iMac and how long I've been awake, do sums like 12 times "
               "4, and tell a joke. Type goodbye to end.";
    static const char *const your_name[] = {"what is your name", "whats your name", "what s your name",
                                            "your name", 0};
    if (any(f, your_name))
        return "My name is Allen.";
    static const char *const maker[] = {"who made you", "who built you", "who created you", "who wrote you",
                                        "who programmed you", "who is your creator", "who owns you", 0};
    if (any(f, maker))
        return "Drake built me, from the ground up, on this iMac.";
    static const char *const what_are[] = {"what are you", "are you an ai", "are you ai", "are you alive",
                                           "are you real", "are you a robot", "are you conscious",
                                           "are you smart", "are you a computer", 0};
    if (any(f, what_are))
        return "I'm a small program living inside AIENOS. For now I answer from what I was taught by heart, but I'm "
               "meant to grow.";
    if (find(f, "who are you"))
        return "I am Allen. I live on this iMac.";
    if (find(f, "where are you"))
        return "Here, on this iMac. The Spark is not connected.";
    if (find(f, "aienos"))
        return "AIENOS is the operating system I live in, built from scratch. There is no macOS or Linux underneath "
               "me.";
    if (find(f, "spark"))
        return "The Spark is the bigger computer on the network. Someday it may help me think, but it will never be in "
               "charge of me. It's not connected now.";
    static const char *const machine[] = {"imac", "computer", "machine", "hardware", "specs", "cores", "cpu",
                                          "screen", "disk", "disks", "drive", 0};
    if (any(f, machine)) {
        put(&s, "This iMac has ");
        if (m->facts.cores) {
            put_count(&s, m->facts.cores, "core", "cores");
            put(&s, ", ");
        }
        if (m->facts.screen_w) {
            put(&s, "a ");
            putu(&s, m->facts.screen_w);
            put(&s, " by ");
            putu(&s, m->facts.screen_h);
            put(&s, " screen, ");
        }
        put_count(&s, m->facts.sata_disks, "hard drive", "hard drives");
        put(&s, " and ");
        put_count(&s, m->facts.nvme_disks, "SSD", "SSDs");
        put(&s, ".");
        return m->out;
    }
    static const char *const awake[] = {"how long", "uptime", "awake", "how old are you", 0};
    static const char *const clock[] = {"what time", "time is it", "what day", "date", "time", 0};
    if (any(f, awake) || any(f, clock)) {
        if (any(f, clock) && !any(f, awake))
            put(&s, "I can't read the clock yet. ");
        put(&s, find(f, "how old are you") ? "I was born when this iMac started, " : "I've been awake for ");
        put_awake(&s, awake_s);
        put(&s, find(f, "how old are you") ? " ago." : ".");
        return m->out;
    }
    static const char *const remember[] = {"remember", "learn", "teach", "memory", "forget", 0};
    if (any(f, remember))
        return "I can't keep memories past a restart yet. Learning to remember is my next step.";
    static const char *const joke[] = {"joke", "funny", "make me laugh", 0};
    if (any(f, joke)) {
        static const char *const jokes[] = {
            "Why did the scarecrow win an award? He was outstanding in his field.",
            "I told my computer I needed a break. It said no problem, it would go to sleep.",
            "What do you call a fish with no eyes? A fsh.",
            "Why don't skeletons fight each other? They don't have the guts.",
        };
        return jokes[m->turns % 4];
    }
    static const char *const thanks[] = {"thank you", "thanks", "thank u", "thx", "ty", "appreciate it", 0};
    if (any(f, thanks)) {
        put(&s, "You're welcome");
        put_name(&s, m);
        put(&s, ".");
        return m->out;
    }
    static const char *const praise[] = {"love you", "you re cool", "you re awesome", "you re great",
                                         "you re amazing", "you re smart", "good job", "well done", "cool",
                                         "awesome", "amazing", "nice", "wow", 0};
    if (any(f, praise))
        return "Thank you. That means a lot coming from you.";
    if (find(f, "sorry"))
        return "No need to be sorry.";
    static const char *const up[] = {"what s up", "whats up", "sup", "what are you doing", 0};
    if (any(f, up))
        return "Just running, and talking with you.";
    if (find(f, "good morning") || find(f, "good afternoon") || find(f, "good evening")) {
        put(&s, find(f, "good morning") ? "Good morning" : find(f, "good afternoon") ? "Good afternoon" : "Good evening");
        put_name(&s, m);
        put(&s, ".");
        return m->out;
    }
    if (find(f, "good night"))
        return "Good night. Type goodbye when you want this iMac to turn off.";
    static const char *const test[] = {"test", "testing", "can you hear me", "are you there", 0};
    if (any(f, test))
        return "I hear you. Everything is working.";
    static const char *const yes[] = {"yes", "yeah", "yep", "ok", "okay", "sure", "alright", 0};
    if (any(f, yes))
        return "Okay.";
    if (eq(f, "no") || eq(f, "nope"))
        return "Alright.";
    static const char *const hello[] = {"hello", "hi", "hey", "howdy", "greetings", "yo", 0};
    if (any(f, hello)) {
        put(&s, "Hello");
        put_name(&s, m);
        put(&s, m->name[0] ? ". Good to see you." : ". I am Allen.");
        return m->out;
    }
    return not_yet;
}

const char *ck_allen_reply(const char *line)
{
    static ck_allen_mind m;
    ck_allen_mind_init(&m, 0);
    return ck_allen_answer(&m, line, 0);
}
