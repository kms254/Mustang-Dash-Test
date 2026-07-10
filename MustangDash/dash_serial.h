/*
 * dash_serial.h -- bench serial protocol: parse + apply (U5, plan KTD6/R13).
 *
 * Pure header, no Arduino/EVE includes, host-testable
 * (see tests/test_dash_serial.c) -- the splash_timeline.h pattern. The .ino
 * accumulates newline-terminated ASCII lines from USB serial (115200) and
 * hands each complete line to dash_parse_line(); dash_apply_command() then
 * writes the sticky override/clear semantics into DashState (dash_data.h).
 *
 * Line protocol (case-insensitive, repeated spaces and a trailing \r are
 * tolerated, max DASH_SERIAL_MAX_LINE chars + NUL):
 *   set <channel> <value>   override a channel (sticky against the sim)
 *   clear <channel>         mark a channel invalid (sticky)
 *   mode track|street
 *   alarm oilp|oilt|clt|off force an alarm condition / release all three
 *   odo set <miles>         caller applies (odometer module owns the value)
 *   sim on|off
 *   status                  caller composes the reply
 *   help                    caller replies DASH_HELP_TEXT
 *
 * Channels: rpm speed ect oilt oilp volts fuel delta lap last best ambient.
 */

#ifndef DASH_SERIAL_H
#define DASH_SERIAL_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "dash_data.h"

/* Longest accepted line, excluding the NUL. Longer -> DASH_ERR_TOO_LONG. */
#define DASH_SERIAL_MAX_LINE 63

typedef enum {
    DASH_CMD_NONE = 0,
    DASH_CMD_SET,
    DASH_CMD_CLEAR,
    DASH_CMD_MODE,
    DASH_CMD_ALARM,
    DASH_CMD_ODO_SET,
    DASH_CMD_SIM,
    DASH_CMD_STATUS,
    DASH_CMD_HELP,
} DashCmdKind;

typedef enum {
    DASH_ERR_NONE = 0,        /* command parsed; act on DashCommand */
    DASH_ERR_EMPTY,           /* blank / whitespace-only line: ignore silently */
    DASH_ERR_UNKNOWN_CMD,     /* first word is not a command */
    DASH_ERR_UNKNOWN_CHANNEL, /* set/clear on a name that is not a channel */
    DASH_ERR_MISSING_VALUE,   /* command is missing a required argument */
    DASH_ERR_BAD_VALUE,       /* argument present but not valid for the command */
    DASH_ERR_RANGE,           /* numeric value outside the channel's range */
    DASH_ERR_TOO_LONG,        /* line longer than DASH_SERIAL_MAX_LINE */
} DashSerialErr;

/* alarm codes carried in DashCommand.alarm -- protocol-side, deliberately
 * NOT named DASH_ALARM_* so they cannot collide with dash_math.h's
 * DashAlarm enum constants (values coincide: 1/2/3, 0 = off) */
#define DASH_SERIAL_ALARM_OFF  0U
#define DASH_SERIAL_ALARM_OILP 1U
#define DASH_SERIAL_ALARM_OILT 2U
#define DASH_SERIAL_ALARM_CLT  3U

/* forced values for the alarm shortcuts (alarm-worthy by design) */
#define DASH_ALARM_OILP_PSI 20.0f
#define DASH_ALARM_OILT_F   260.0f
#define DASH_ALARM_CLT_F    230.0f

#define DASH_HELP_TEXT \
    "commands: set <ch> <v> | clear <ch> | mode track|street | " \
    "alarm oilp|oilt|clt|off | odo set <miles> | sim on|off | status | help " \
    "(ch: rpm speed ect oilt oilp volts fuel delta lap last best ambient)"

typedef struct {
    DashCmdKind kind;
    uint8_t channel; /* SET / CLEAR: a DASH_CH_ id */
    float value;     /* SET: channel value; ODO_SET: miles */
    DashMode mode;   /* MODE */
    uint8_t alarm;   /* ALARM: DASH_SERIAL_ALARM_OFF/OILP/OILT/CLT */
    bool sim_on;     /* SIM */
} DashCommand;

/* ---- internals ---- */

static inline char dash_serial_lc_(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char) (c + ('a' - 'A')) : c;
}

/* case-insensitive equality, ASCII only (the whole protocol is ASCII) */
static inline bool dash_serial_ieq_(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (dash_serial_lc_(*a) != dash_serial_lc_(*b)) { return false; }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Protocol name for a channel id ("?" for anything else). */
static inline const char *dash_ch_name(uint8_t ch)
{
    switch (ch) {
        case DASH_CH_RPM: return "rpm";
        case DASH_CH_SPEED: return "speed";
        case DASH_CH_ECT: return "ect";
        case DASH_CH_OILT: return "oilt";
        case DASH_CH_OILP: return "oilp";
        case DASH_CH_VOLTS: return "volts";
        case DASH_CH_FUEL: return "fuel";
        case DASH_CH_DELTA: return "delta";
        case DASH_CH_LAP: return "lap";
        case DASH_CH_LAST: return "last";
        case DASH_CH_BEST: return "best";
        case DASH_CH_AMBIENT: return "ambient";
        default: return "?";
    }
}

/* Channel id for a protocol name, or -1 when unknown. */
static inline int dash_ch_from_name_(const char *name)
{
    for (uint8_t ch = 0; ch < DASH_CH_COUNT; ch++) {
        if (dash_serial_ieq_(name, dash_ch_name(ch))) { return (int) ch; }
    }
    return -1;
}

/* Accepted [lo, hi] for a channel's `set` value (plan KTD6 ranges). */
static inline void dash_ch_range_(uint8_t ch, float *lo, float *hi)
{
    switch (ch) {
        case DASH_CH_RPM: *lo = 0.0f; *hi = 12000.0f; break;
        case DASH_CH_SPEED: *lo = 0.0f; *hi = 300.0f; break;
        case DASH_CH_ECT: /* fall through: temperatures share a range */
        case DASH_CH_OILT: *lo = -40.0f; *hi = 400.0f; break;
        case DASH_CH_OILP: *lo = 0.0f; *hi = 150.0f; break;
        case DASH_CH_VOLTS: *lo = 0.0f; *hi = 20.0f; break;
        case DASH_CH_FUEL: *lo = 0.0f; *hi = 20.0f; break;
        case DASH_CH_DELTA: *lo = -30.0f; *hi = 30.0f; break;
        case DASH_CH_LAP: /* fall through: lap times share a range (ms) */
        case DASH_CH_LAST:
        case DASH_CH_BEST: *lo = 0.0f; *hi = 3600000.0f; break;
        case DASH_CH_AMBIENT: *lo = -40.0f; *hi = 150.0f; break;
        default: *lo = 0.0f; *hi = 0.0f; break;
    }
}

/* Strict float parse: the whole token must be a number. */
static inline bool dash_serial_parse_float_(const char *tok, float *out)
{
    char *end = NULL;
    float v = strtof(tok, &end);
    if (end == tok || *end != '\0') { return false; }
    *out = v;
    return true;
}

/* Split buf in place on spaces/tabs/CR/LF; store up to max tokens. */
static inline int dash_serial_tokenize_(char *buf, char *tok[], int max)
{
    int n = 0;
    char *p = buf;
    while (*p != '\0' && n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; }
        if (*p == '\0') { break; }
        tok[n++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') { p++; }
        if (*p != '\0') { *p++ = '\0'; }
    }
    return n;
}

/* ---- API ---- */

/* Parse one accumulated line (no need to strip \r or \n first) into *out.
 * Returns DASH_ERR_NONE on success; *out is only meaningful then. */
static inline DashSerialErr dash_parse_line(const char *line, DashCommand *out)
{
    char buf[DASH_SERIAL_MAX_LINE + 1];
    char *tok[4] = { NULL, NULL, NULL, NULL };
    int ntok;

    out->kind = DASH_CMD_NONE;
    out->channel = 0U;
    out->value = 0.0f;
    out->mode = DASH_MODE_TRACK;
    out->alarm = DASH_SERIAL_ALARM_OFF;
    out->sim_on = false;

    if (line == NULL) { return DASH_ERR_EMPTY; }
    if (strlen(line) > DASH_SERIAL_MAX_LINE) { return DASH_ERR_TOO_LONG; }
    strcpy(buf, line);

    ntok = dash_serial_tokenize_(buf, tok, 4);
    if (ntok == 0) { return DASH_ERR_EMPTY; }

    if (dash_serial_ieq_(tok[0], "set") || dash_serial_ieq_(tok[0], "clear")) {
        bool is_set = dash_serial_ieq_(tok[0], "set");
        int ch;
        if (ntok < 2) { return DASH_ERR_MISSING_VALUE; }
        ch = dash_ch_from_name_(tok[1]);
        if (ch < 0) { return DASH_ERR_UNKNOWN_CHANNEL; }
        if (!is_set) {
            out->kind = DASH_CMD_CLEAR;
            out->channel = (uint8_t) ch;
            return DASH_ERR_NONE;
        }
        if (ntok < 3) { return DASH_ERR_MISSING_VALUE; }
        {
            float v, lo, hi;
            if (!dash_serial_parse_float_(tok[2], &v)) { return DASH_ERR_BAD_VALUE; }
            dash_ch_range_((uint8_t) ch, &lo, &hi);
            if (!(v >= lo && v <= hi)) { return DASH_ERR_RANGE; } /* NaN -> RANGE */
            out->kind = DASH_CMD_SET;
            out->channel = (uint8_t) ch;
            out->value = v;
        }
        return DASH_ERR_NONE;
    }

    if (dash_serial_ieq_(tok[0], "mode")) {
        if (ntok < 2) { return DASH_ERR_MISSING_VALUE; }
        if (dash_serial_ieq_(tok[1], "track")) { out->mode = DASH_MODE_TRACK; }
        else if (dash_serial_ieq_(tok[1], "street")) { out->mode = DASH_MODE_STREET; }
        else { return DASH_ERR_BAD_VALUE; }
        out->kind = DASH_CMD_MODE;
        return DASH_ERR_NONE;
    }

    if (dash_serial_ieq_(tok[0], "alarm")) {
        if (ntok < 2) { return DASH_ERR_MISSING_VALUE; }
        if (dash_serial_ieq_(tok[1], "oilp")) { out->alarm = DASH_SERIAL_ALARM_OILP; }
        else if (dash_serial_ieq_(tok[1], "oilt")) { out->alarm = DASH_SERIAL_ALARM_OILT; }
        else if (dash_serial_ieq_(tok[1], "clt")) { out->alarm = DASH_SERIAL_ALARM_CLT; }
        else if (dash_serial_ieq_(tok[1], "off")) { out->alarm = DASH_SERIAL_ALARM_OFF; }
        else { return DASH_ERR_BAD_VALUE; }
        out->kind = DASH_CMD_ALARM;
        return DASH_ERR_NONE;
    }

    if (dash_serial_ieq_(tok[0], "odo")) {
        float v;
        if (ntok < 2) { return DASH_ERR_MISSING_VALUE; }
        if (!dash_serial_ieq_(tok[1], "set")) { return DASH_ERR_BAD_VALUE; }
        if (ntok < 3) { return DASH_ERR_MISSING_VALUE; }
        if (!dash_serial_parse_float_(tok[2], &v)) { return DASH_ERR_BAD_VALUE; }
        if (!(v >= 0.0f && v <= 2000000.0f)) { return DASH_ERR_RANGE; }
        out->kind = DASH_CMD_ODO_SET;
        out->value = v;
        return DASH_ERR_NONE;
    }

    if (dash_serial_ieq_(tok[0], "sim")) {
        if (ntok < 2) { return DASH_ERR_MISSING_VALUE; }
        if (dash_serial_ieq_(tok[1], "on")) { out->sim_on = true; }
        else if (dash_serial_ieq_(tok[1], "off")) { out->sim_on = false; }
        else { return DASH_ERR_BAD_VALUE; }
        out->kind = DASH_CMD_SIM;
        return DASH_ERR_NONE;
    }

    if (dash_serial_ieq_(tok[0], "status")) {
        out->kind = DASH_CMD_STATUS;
        return DASH_ERR_NONE;
    }

    if (dash_serial_ieq_(tok[0], "help")) {
        out->kind = DASH_CMD_HELP;
        return DASH_ERR_NONE;
    }

    return DASH_ERR_UNKNOWN_CMD;
}

/* SET semantics shared by `set` and the `alarm` shortcuts: write the value,
 * mark valid + overridden (sim keeps hands off), drop any sticky clear. */
static inline void dash_serial_do_set_(DashState *s, uint8_t ch, float v)
{
    uint16_t bit = DASH_CH_BIT(ch);
    dash_ch_set(s, ch, v);
    s->overridden |= bit;
    s->cleared = (uint16_t) (s->cleared & ~bit);
}

/* Applies everything except ODO_SET/STATUS/HELP (caller handles those --
 * the odometer module owns the persisted miles, and status/help replies are
 * composed from state the caller has). Writes a one-line reply (no newline)
 * into reply[reply_len], truncating safely. Returns true when handled. */
static inline bool dash_apply_command(DashState *s, const DashCommand *cmd,
                                      char *reply, size_t reply_len)
{
    bool can_reply = (reply != NULL && reply_len > 0U);
    if (can_reply) { reply[0] = '\0'; }

    switch (cmd->kind) {
        case DASH_CMD_SET:
            dash_serial_do_set_(s, cmd->channel, cmd->value);
            if (can_reply) {
                snprintf(reply, reply_len, "ok set %s %g",
                         dash_ch_name(cmd->channel), (double) cmd->value);
            }
            return true;

        case DASH_CMD_CLEAR: {
            uint16_t bit = DASH_CH_BIT(cmd->channel);
            s->valid = (uint16_t) (s->valid & ~bit);
            s->cleared |= bit;
            s->overridden = (uint16_t) (s->overridden & ~bit);
            if (can_reply) {
                snprintf(reply, reply_len, "ok clear %s", dash_ch_name(cmd->channel));
            }
            return true;
        }

        case DASH_CMD_MODE:
            s->mode = cmd->mode;
            if (can_reply) {
                snprintf(reply, reply_len, "ok mode %s",
                         (cmd->mode == DASH_MODE_STREET) ? "street" : "track");
            }
            return true;

        case DASH_CMD_ALARM: {
            const char *which = "off";
            switch (cmd->alarm) {
                case DASH_SERIAL_ALARM_OILP:
                    dash_serial_do_set_(s, DASH_CH_OILP, DASH_ALARM_OILP_PSI);
                    which = "oilp";
                    break;
                case DASH_SERIAL_ALARM_OILT:
                    dash_serial_do_set_(s, DASH_CH_OILT, DASH_ALARM_OILT_F);
                    which = "oilt";
                    break;
                case DASH_SERIAL_ALARM_CLT:
                    dash_serial_do_set_(s, DASH_CH_ECT, DASH_ALARM_CLT_F);
                    which = "clt";
                    break;
                default: { /* off: release all three alarm channels, valid
                            * bits included -- otherwise a forced value stays
                            * valid and the takeover latches when the sim is
                            * frozen. Sim running: it revalidates them next
                            * step. Sim frozen: they render `--` (the
                            * missing-data convention), which beats showing a
                            * stale forced 20 psi. */
                    uint16_t bits = (uint16_t) (DASH_CH_BIT(DASH_CH_OILP) |
                                                DASH_CH_BIT(DASH_CH_OILT) |
                                                DASH_CH_BIT(DASH_CH_ECT));
                    s->overridden = (uint16_t) (s->overridden & ~bits);
                    s->cleared = (uint16_t) (s->cleared & ~bits);
                    s->valid = (uint16_t) (s->valid & ~bits);
                    break;
                }
            }
            if (can_reply) { snprintf(reply, reply_len, "ok alarm %s", which); }
            return true;
        }

        case DASH_CMD_SIM:
            if (cmd->sim_on) {
                s->overridden = 0U;
                s->cleared = 0U;
                s->sim_frozen = false;
            } else {
                s->sim_frozen = true;
            }
            if (can_reply) {
                snprintf(reply, reply_len, "ok sim %s", cmd->sim_on ? "on" : "off");
            }
            return true;

        default: /* NONE, ODO_SET, STATUS, HELP: the caller composes these */
            return false;
    }
}

#endif /* DASH_SERIAL_H */
