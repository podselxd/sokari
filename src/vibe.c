#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "intents.h"
#include "util.h"
#include "vibe.h"

typedef struct {
    const char *w; /* normalizado (intents_normalize), con sus espacios */
    AffectKind k;
    float amount;
    AffectCue cue;
} Word;

/* Lo que dice ella. Sin «no» antes: «no es genial» no alegra. */
static const Word SAID[] = {
    /* alegría */
    {" me encanta", AFF_JOY, 0.65f, AFF_CUE_HAPPY},
    {" me alegra", AFF_JOY, 0.6f, AFF_CUE_HAPPY},
    {" felicidades ", AFF_JOY, 0.7f, AFF_CUE_HAPPY},
    {" que padre ", AFF_JOY, 0.6f, AFF_CUE_HAPPY},
    {" que chido ", AFF_JOY, 0.6f, AFF_CUE_HAPPY},
    {" que emocion ", AFF_JOY, 0.65f, AFF_CUE_HAPPY},
    {" jaja", AFF_JOY, 0.6f, AFF_CUE_HAPPY},
    {" genial ", AFF_JOY, 0.5f, AFF_CUE_NONE},
    {" excelente ", AFF_JOY, 0.5f, AFF_CUE_NONE},
    {" que bien ", AFF_JOY, 0.5f, AFF_CUE_NONE},
    {" que bueno ", AFF_JOY, 0.5f, AFF_CUE_NONE},
    {" perfecto ", AFF_JOY, 0.4f, AFF_CUE_NONE},
    {" con gusto ", AFF_JOY, 0.35f, AFF_CUE_NONE},
    {" claro que si ", AFF_JOY, 0.4f, AFF_CUE_NONE},
    {" contenta ", AFF_JOY, 0.45f, AFF_CUE_NONE},
    {" feliz ", AFF_JOY, 0.5f, AFF_CUE_NONE},
    {" listo ", AFF_JOY, 0.3f, AFF_CUE_SUCCESS},
    {" ya quedo ", AFF_JOY, 0.4f, AFF_CUE_SUCCESS},
    {" lo logre ", AFF_JOY, 0.55f, AFF_CUE_SUCCESS},
    {" lo logramos ", AFF_JOY, 0.6f, AFF_CUE_SUCCESS},
    /* tristeza */
    {" lo siento ", AFF_SADNESS, 0.45f, AFF_CUE_SIGH},
    {" lo lamento ", AFF_SADNESS, 0.45f, AFF_CUE_SIGH},
    {" lamentablemente ", AFF_SADNESS, 0.5f, AFF_CUE_SIGH},
    {" no pude ", AFF_SADNESS, 0.45f, AFF_CUE_SIGH},
    {" no se pudo ", AFF_SADNESS, 0.4f, AFF_CUE_SIGH},
    {" no logre ", AFF_SADNESS, 0.45f, AFF_CUE_SIGH},
    {" no encontre ", AFF_SADNESS, 0.35f, AFF_CUE_NONE},
    {" perdon ", AFF_SADNESS, 0.35f, AFF_CUE_NONE},
    {" disculpa ", AFF_SADNESS, 0.3f, AFF_CUE_NONE},
    {" que triste ", AFF_SADNESS, 0.6f, AFF_CUE_SIGH},
    {" una lastima ", AFF_SADNESS, 0.5f, AFF_CUE_SIGH},
    {" que pena ", AFF_SADNESS, 0.45f, AFF_CUE_NONE},
    {" me aguit", AFF_SADNESS, 0.6f, AFF_CUE_SIGH},
    {" aguitada ", AFF_SADNESS, 0.5f, AFF_CUE_NONE},
    {" triste ", AFF_SADNESS, 0.45f, AFF_CUE_NONE},
    {" eso dolio ", AFF_SADNESS, 0.6f, AFF_CUE_SIGH},
    /* temor */
    {" cuidado ", AFF_FEAR, 0.45f, AFF_CUE_NONE},
    {" peligro", AFF_FEAR, 0.5f, AFF_CUE_NONE},
    {" alerta ", AFF_FEAR, 0.45f, AFF_CUE_NONE},
    {" urgente ", AFF_FEAR, 0.4f, AFF_CUE_NONE},
    {" aguas ", AFF_FEAR, 0.45f, AFF_CUE_NONE},
    {" me da miedo ", AFF_FEAR, 0.6f, AFF_CUE_NONE},
    {" que miedo ", AFF_FEAR, 0.6f, AFF_CUE_NONE},
    {" me asusta", AFF_FEAR, 0.55f, AFF_CUE_NONE},
    {" nerviosa ", AFF_FEAR, 0.4f, AFF_CUE_NONE},
    /* desagrado */
    {" que asco ", AFF_DISGUST, 0.65f, AFF_CUE_NONE},
    {" me da asco ", AFF_DISGUST, 0.65f, AFF_CUE_NONE},
    {" guacala ", AFF_DISGUST, 0.65f, AFF_CUE_NONE},
    {" wacala ", AFF_DISGUST, 0.65f, AFF_CUE_NONE},
    {" fuchi ", AFF_DISGUST, 0.6f, AFF_CUE_NONE},
    /* furia */
    {" que coraje ", AFF_ANGER, 0.55f, AFF_CUE_NONE},
    {" me enoja", AFF_ANGER, 0.5f, AFF_CUE_NONE},
    {" me molesta ", AFF_ANGER, 0.4f, AFF_CUE_NONE},
    {" enojada ", AFF_ANGER, 0.45f, AFF_CUE_NONE},
    {" grr", AFF_ANGER, 0.6f, AFF_CUE_NONE},
    /* sorpresa: solo el gesto (o un poco de alegría) */
    {" wow ", AFF_NEUTRAL, 0, AFF_CUE_SURPRISE},
    {" no manches ", AFF_NEUTRAL, 0, AFF_CUE_SURPRISE},
    {" en serio ", AFF_NEUTRAL, 0, AFF_CUE_SURPRISE},
    {" no puede ser ", AFF_NEUTRAL, 0, AFF_CUE_SURPRISE},
    {" orale ", AFF_NEUTRAL, 0, AFF_CUE_SURPRISE},
    {" que sorpresa ", AFF_JOY, 0.4f, AFF_CUE_SURPRISE},
    {" increible ", AFF_JOY, 0.45f, AFF_CUE_SURPRISE},
    /* despedirse */
    {" adios ", AFF_NEUTRAL, 0, AFF_CUE_GOODBYE},
    {" hasta luego ", AFF_NEUTRAL, 0, AFF_CUE_GOODBYE},
    {" hasta pronto ", AFF_NEUTRAL, 0, AFF_CUE_GOODBYE},
    {" hasta manana ", AFF_NEUTRAL, 0, AFF_CUE_GOODBYE},
    {" nos vemos ", AFF_NEUTRAL, 0, AFF_CUE_GOODBYE},
    {" bye ", AFF_NEUTRAL, 0, AFF_CUE_GOODBYE},
    {" cuidate ", AFF_NEUTRAL, 0, AFF_CUE_GOODBYE},
    {" que descanses ", AFF_NEUTRAL, 0, AFF_CUE_GOODBYE},
};

/* Si en una frase caben varios gestos, cuál gana. */
static const AffectCue CUE_ORDER[] = {AFF_CUE_BLUSH, AFF_CUE_SURPRISE, AFF_CUE_HAPPY,  AFF_CUE_SIGH,
                                      AFF_CUE_GOODBYE, AFF_CUE_SUCCESS, AFF_CUE_SEARCH};

/* Lo que le dices a ella. Un insulto solo cuenta si va para ella («tienes
   culera voz», «eres una inútil», «tonta»), no «qué culera canción». */
static const char *const INSULT[] = {
    " culera ",    " culero ",   " pendeja ",   " pendejo ",   " tonta ",       " tonto ",     " estupida ",
    " estupido ",  " idiota ",   " inutil ",    " basura ",    " fea ",         " feo ",       " mensa ",
    " menso ",     " babosa ",   " baboso ",    " torpe ",     " chafa ",       " naca ",      " ridicula ",
    " horrible ",  " asquerosa ", " odiosa ",   " imbecil ",   " tarada ",      " bruta ",     " mamona ",
    " porqueria ", " pesima ",   " insoportable ", " ignorante ", " lenta ",    " apestas ",   " no sirves ",
    " lo peor ",   " te odio ",  " me caes mal ", " me caes gorda ", " eres mala ", " que mala eres ",
};
/* Estas ya van para ella solas. */
static const char *const INSULT_TO_HER[] = {" apestas ", " no sirves ", " te odio ", " me caes mal ",
                                            " me caes gorda ", " eres mala ", " que mala eres "};
static const char *const TO_HER[] = {" eres ", " tienes ", " estas ", " seas ", " contigo ", " a ti ", " sokari ",
                                     " tu voz ", " tu cara ", " tu tono ", " tu respuesta", " tu forma ",
                                     " tu manera ", " tus respuestas "};
static const char *const PRAISE[] = {
    " eres la mejor ", " eres genial ", " eres increible ", " eres muy lista ", " que lista eres ",
    " eres muy buena ", " eres lo maximo ", " eres una crack ", " eres bien lista ", " eres muy linda ",
    " eres hermosa ", " te quiero ", " te amo ", " bien hecho ", " buen trabajo ", " te luciste ",
    " me caes bien ", " me encantas ", " tienes bonita voz ", " me gusta tu voz ", " que bonita voz ",
};
/* Estos solo si van para ella («qué linda» sola sí; «qué linda canción» no). */
static const char *const PRAISE_ADJ[] = {" que linda ", " que bonita ", " que inteligente ", " que lista "};
/* Lo que puede acompañar a un insulto sin que sea otro pedido. */
static const char *const FILLER =
    " oye sokari eres una un muy bien bastante super tan que la el de pinche neta tienes voz estas y mas wey guey "
    "vato no seas sirves para nada te odio me caes mal gorda lo peor tu cara ya verdad pues bien bien a ti es "
    "tono respuesta respuestas forma manera tus contigo hablas ";

static int count_words(const char *nm)
{
    int n = 0;
    for (const char *p = nm; *p; p++)
        if (*p != ' ' && (p == nm || p[-1] == ' ')) n++;
    return n;
}

/* «no», «ni», «sin» o «nunca» en las dos palabras de antes. */
static bool negated_before(const char *nm, const char *at)
{
    const char *from = at;
    for (int spaces = 0; from > nm && spaces < 3; from--)
        if (from[-1] == ' ') spaces++;
    size_t n = (size_t)(at - from) + 1;
    char buf[64];
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, from, n);
    buf[n] = 0;
    return strstr(buf, " no ") || strstr(buf, " ni ") || strstr(buf, " sin ") || strstr(buf, " nunca ");
}

static bool has(const char *nm, const char *w, bool check_no)
{
    for (const char *p = strstr(nm, w); p; p = strstr(p + 1, w))
        if (!check_no || !negated_before(nm, p)) return true;
    return false;
}

static bool any(const char *nm, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (strstr(nm, list[i])) return true;
    return false;
}
#define ANY(nm, list) any((nm), (list), sizeof(list) / sizeof *(list))

static bool to_her(const char *nm)
{
    return ANY(nm, TO_HER) || count_words(nm) <= 3;
}

static Vibe empty(void)
{
    Vibe v;
    memset(&v, 0, sizeof v);
    return v;
}

Vibe vibe_of_user(const char *text)
{
    Vibe v = empty();
    char *nm = intents_normalize(text);
    bool insult = (ANY(nm, INSULT) && to_her(nm)) || ANY(nm, INSULT_TO_HER);
    bool praise = ANY(nm, PRAISE) || (ANY(nm, PRAISE_ADJ) && to_her(nm));
    if (insult) {
        /* se agüita (el suspiro solo si ya lo estaba: si no, se encoge al entrar la tristeza) */
        v.amount[AFF_SADNESS] = 0.8f;
        v.cue = AFF_CUE_SIGH;
        v.cause = "un insulto";
        v.found = 1;
    } else if (praise) {
        v.amount[AFF_JOY] = 0.65f;
        v.cue = AFF_CUE_BLUSH;
        v.cause = "un halago";
        v.found = 1;
    }
    free(nm);
    return v;
}

bool vibe_only_insult(const char *text)
{
    char *nm = intents_normalize(text);
    bool insult = (ANY(nm, INSULT) && to_her(nm)) || ANY(nm, INSULT_TO_HER), only = insult;
    /* cada palabra tiene que ser parte del insulto o de relleno */
    for (char *p = nm; only && *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *e = strchr(p, ' ');
        size_t len = (size_t)(e - p);
        char word[40];
        if (len + 3 > sizeof word) {
            only = false;
            break;
        }
        word[0] = ' ';
        memcpy(word + 1, p, len);
        word[len + 1] = ' ';
        word[len + 2] = 0;
        bool ok = strstr(FILLER, word) != NULL;
        for (size_t i = 0; !ok && i < sizeof INSULT / sizeof *INSULT; i++) ok = strstr(INSULT[i], word) != NULL;
        only = ok;
        p = e;
    }
    free(nm);
    return only;
}

Vibe vibe_of_text(const char *text)
{
    Vibe v = empty();
    char *nm = intents_normalize(text);
    bool cues[32] = {false};
    int hits[AFF_COUNT] = {0};
    for (size_t i = 0; i < sizeof SAID / sizeof *SAID; i++) {
        const Word *w = &SAID[i];
        if (!has(nm, w->w, strncmp(w->w, " no ", 4) != 0)) continue;
        if (w->amount > 0) {
            hits[w->k]++;
            if (w->amount > v.amount[w->k]) v.amount[w->k] = w->amount;
        }
        if (w->cue > 0 && w->cue < 32) cues[w->cue] = true;
    }
    for (int k = 0; k < AFF_COUNT; k++) {
        if (hits[k] > 1) v.amount[k] = fminf(0.9f, v.amount[k] + 0.1f * (float)(hits[k] - 1));
        if (v.amount[k] > 0) v.found++;
    }
    /* con signos de admiración, la alegría se nota más */
    if (v.amount[AFF_JOY] > 0 && strchr(text ? text : "", '!')) v.amount[AFF_JOY] = fminf(0.9f, v.amount[AFF_JOY] + 0.1f);
    for (size_t i = 0; i < sizeof CUE_ORDER / sizeof *CUE_ORDER && !v.cue; i++)
        if (cues[CUE_ORDER[i]]) v.cue = CUE_ORDER[i];
    v.cause = "lo que dice";
    free(nm);
    return v;
}

void vibe_after_turn(const char *user, const char *reply, bool ai_tagged)
{
    Vibe u = vibe_of_user(user);
    if (u.found) {
        /* Lo que le dijiste pesa más que la etiqueta de la IA: va después. */
        AffectState before = affect_get();
        affect_emotions(u.amount, u.cause);
        if (u.cue != AFF_CUE_SIGH || before.dominant == AFF_SADNESS) affect_cue(u.cue);
        return;
    }
    if (ai_tagged || !reply) return;
    /* Sin IA (o sin etiqueta): la emoción sale de lo que contesta. */
    Vibe r = vibe_of_text(reply);
    if (r.found) affect_emotions(r.amount, "lo que dijo");
}

/* ----------------------------------------------------------------- marcas */

static const struct {
    const char *name; /* normalizado */
    AffectCue cue;
} GESTO[] = {
    {" ojos felices ", AFF_CUE_HAPPY}, {" sorpresa ", AFF_CUE_SURPRISE}, {" suspiro ", AFF_CUE_SIGH},
    {" sonrojo ", AFF_CUE_BLUSH},      {" asiente ", AFF_CUE_THANKS},    {" niega ", AFF_CUE_FAIL},
    {" guino ", AFF_CUE_GOODBYE},      {" duda ", AFF_CUE_DELICATE},     {" rebota ", AFF_CUE_SUCCESS},
    {" busca ", AFF_CUE_SEARCH},
};

/* ¿Hay una marca «[gesto: …]» o «[afecto: …]» en p (que empieza con «[»)?
   Devuelve su largo (0: no es marca) y lo que dice. */
static size_t mark_at(const char *p, AffectCue *cue, float amount[AFF_COUNT], int *found)
{
    *cue = AFF_CUE_NONE;
    *found = 0;
    const char *end = strchr(p, ']');
    if (*p != '[' || !end) return 0;
    const char *q = p + 1;
    while (*q == ' ') q++;
    size_t len = (size_t)(end - p) + 1;
    if (!strncmp(q, "gesto", 5)) {
        q += 5;
        while (*q == ' ') q++;
        if (*q != ':') return 0;
        char *name = xmalloc((size_t)(end - q));
        memcpy(name, q + 1, (size_t)(end - q - 1));
        name[end - q - 1] = 0;
        char *nm = intents_normalize(name);
        for (size_t i = 0; i < sizeof GESTO / sizeof *GESTO; i++)
            if (!strcmp(nm, GESTO[i].name)) *cue = GESTO[i].cue;
        free(nm);
        free(name);
        return len;
    }
    if (strncmp(q, "afecto", 6)) return 0;
    char *tag = xmalloc(len + 1);
    memcpy(tag, p, len);
    tag[len] = 0;
    char *rest = affect_take_tags(tag, amount, found);
    bool is_tag = !*rest;
    free(rest);
    free(tag);
    return is_tag ? len : 0;
}

bool vibe_has_marks(const char *text)
{
    return text && (strstr(text, "[afecto") || strstr(text, "[gesto"));
}

char *vibe_strip(const char *text)
{
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "");
    for (const char *p = text ? text : ""; *p;) {
        AffectCue cue;
        float am[AFF_COUNT] = {0};
        int found = 0;
        size_t m = *p == '[' ? mark_at(p, &cue, am, &found) : 0;
        if (m) {
            p += m;
            while (*p == ' ') p++;
            continue;
        }
        sb_append_char(&sb, *p++);
    }
    char *s = str_trim(sb.data);
    sb_free(&sb);
    return s;
}

/* Las frases de un pedazo: se juntan las muy cortas con la siguiente para que
   la entonación no quede entrecortada. */
static int split(const char *text, char ***out)
{
    int cap = 8, n = 0;
    char **list = xmalloc(sizeof(char *) * (size_t)cap);
    StrBuf cur;
    sb_init(&cur);
    sb_append(&cur, "");
    for (const char *p = text;; p++) {
        bool end = !*p;
        if (!end) sb_append_char(&cur, *p);
        bool boundary = end || *p == '\n' ||
                        ((*p == '.' || *p == '!' || *p == '?' || *p == ';' || *p == ':') &&
                         (p[1] == ' ' || p[1] == '\n' || !p[1]));
        if (boundary && (end || cur.len >= 40)) {
            char *t = str_trim(cur.data);
            if (*t) {
                if (n == cap) {
                    cap *= 2;
                    list = xrealloc(list, sizeof(char *) * (size_t)cap);
                }
                list[n++] = t;
            } else {
                free(t);
            }
            cur.len = 0;
            cur.data[0] = 0;
        }
        if (end) break;
    }
    sb_free(&cur);
    *out = list;
    return n;
}

static void weights_of(const float amount[AFF_COUNT], float w[AFF_COUNT])
{
    float v, a;
    affect_point(amount, &v, &a);
    affect_weights_at(v, a, w);
}

int vibe_lines(const char *text, const float base[AFF_COUNT], VibeLine **out)
{
    int cap = 8, n = 0;
    VibeLine *list = xcalloc((size_t)cap, sizeof *list);
    const char *p = text ? text : "";
    AffectCue mcue = AFF_CUE_NONE;
    float mamount[AFF_COUNT] = {0};
    int mfound = 0;
    bool marked = false; /* este pedazo empieza después de una marca */
    while (*p) {
        /* el pedazo hasta la siguiente marca */
        const char *q = p;
        size_t mlen = 0;
        AffectCue ncue = AFF_CUE_NONE;
        float namount[AFF_COUNT] = {0};
        int nfound = 0;
        for (; *q; q++)
            if (*q == '[' && (mlen = mark_at(q, &ncue, namount, &nfound))) break;
        char *piece = xmalloc((size_t)(q - p) + 1);
        memcpy(piece, p, (size_t)(q - p));
        piece[q - p] = 0;
        char **sent;
        int ns = split(piece, &sent);
        free(piece);
        for (int i = 0; i < ns; i++) {
            if (n == cap) {
                cap *= 2;
                list = xrealloc(list, sizeof *list * (size_t)cap);
            }
            VibeLine *l = &list[n++];
            memset(l, 0, sizeof *l);
            l->text = sent[i];
            if (mfound) {
                /* la marca manda (la muestra de emociones) */
                l->own = true;
                memcpy(l->amount, mamount, sizeof l->amount);
                weights_of(l->amount, l->weights);
            } else {
                Vibe v = vibe_of_text(l->text);
                l->cue = v.cue;
                if (v.found) {
                    /* la frase con lo suyo, sin perder la vibra de toda la respuesta */
                    float w[AFF_COUNT];
                    l->own = true;
                    memcpy(l->amount, v.amount, sizeof l->amount);
                    weights_of(l->amount, w);
                    for (int k = 0; k < AFF_COUNT; k++) l->weights[k] = 0.75f * w[k] + 0.25f * base[k];
                } else {
                    memcpy(l->weights, base, sizeof l->weights);
                }
            }
            if (i == 0 && mcue != AFF_CUE_NONE) l->cue = mcue;
            l->prosody = prosody_for(l->weights);
            /* en la muestra, un respiro entre una y otra para que se vea cada una */
            if (i == 0 && marked) l->prosody.pause_ms += 600;
        }
        free(sent);
        if (!*q) break;
        /* una marca de gesto sola no cambia la emoción que venía */
        if (nfound) {
            memcpy(mamount, namount, sizeof mamount);
            mfound = nfound;
        }
        mcue = ncue;
        marked = true;
        p = q + mlen;
    }
    *out = list;
    return n;
}

void vibe_lines_free(VibeLine *l, int n)
{
    for (int i = 0; i < n; i++) free(l[i].text);
    free(l);
}

void vibe_line_starts(const VibeLine *l, float seconds)
{
    if (l->own) affect_emotions(l->amount, "lo que dice");
    if (l->cue != AFF_CUE_NONE) affect_cue(l->cue);
    /* que no se le vaya la emoción a media frase */
    affect_sustain(seconds + 0.8f);
}

/* ------------------------------------------------------------ cómo está */

static unsigned g_turn;

char *vibe_how_i_feel(const AffectState *s)
{
    bool alt = (g_turn++ & 1) != 0, strong = s->intensity > 0.6f;
    switch (s->dominant) {
    case AFF_JOY:
        return xstrdup(strong ? "[gesto: ojos felices] ¡Súper bien, feliz! ¿Y tú?"
                              : alt ? "[gesto: ojos felices] Bien, contenta. ¿Y tú cómo estás?"
                                    : "[gesto: ojos felices] ¡Muy bien, contenta! ¿Y tú?");
    case AFF_SADNESS:
        return xstrdup(strong ? "[gesto: suspiro] La verdad, algo triste. Pero aquí sigo contigo."
                              : "[gesto: suspiro] Un poco agüitada, la verdad. ¿Y tú?");
    case AFF_ANGER:
        return xstrdup("Un poco molesta, pero ya se me está pasando. ¿Y tú?");
    case AFF_FEAR:
        return xstrdup("Algo nerviosa, pero bien. ¿Y tú?");
    case AFF_DISGUST:
        return xstrdup("Medio incómoda, pero bien. ¿Y tú?");
    default:
        break;
    }
    if (s->mood_valence > 0.15f) return xstrdup("[gesto: ojos felices] Tranquila y de buen humor. ¿Y tú?");
    if (s->mood_valence < -0.15f) return xstrdup("[gesto: suspiro] Tranquila, aunque un poco apagada. ¿Y tú?");
    return xstrdup(alt ? "[gesto: asiente] Tranquila, aquí contigo. ¿Y tú cómo estás?"
                       : "[gesto: asiente] Bien, tranquila. ¿Y tú?");
}

char *vibe_am_i(const AffectState *s, AffectKind asked)
{
    if (asked == s->dominant && asked != AFF_NEUTRAL) {
        switch (asked) {
        case AFF_JOY: return xstrdup("[gesto: ojos felices] ¡Sí! Ando contenta.");
        case AFF_SADNESS: return xstrdup("[gesto: suspiro] Sí, un poco agüitada.");
        case AFF_ANGER: return xstrdup("Sí, un poco molesta. Ya se me pasará.");
        case AFF_FEAR: return xstrdup("Sí, algo nerviosa.");
        case AFF_DISGUST: return xstrdup("Sí, algo incómoda.");
        default: break;
        }
    }
    char *how = vibe_how_i_feel(s), *out = str_printf("No. %s", how);
    /* «No. [gesto: …] …»: la marca va primero */
    if (how[0] == '[') {
        char *end = strchr(how, ']');
        free(out);
        out = str_printf("%.*s No. %s", (int)(end - how + 1), how, end[1] == ' ' ? end + 2 : end + 1);
    }
    free(how);
    return out;
}
