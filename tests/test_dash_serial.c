/*
 * Invariant test: the bench serial protocol (U5, plan KTD6/R13) -- line
 * parsing, per-channel value ranges, error codes, and the sticky
 * override/clear semantics dash_apply_command() writes into DashState.
 * Pins the protocol so a future edit cannot silently change what the
 * bench commands mean.
 *
 * Runs on the host:
 *   gcc -std=c11 -I MustangDash tests/test_dash_serial.c -lm -o /tmp/tds && /tmp/tds
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "dash_serial.h"

/* protocol constants that must never drift */
_Static_assert(DASH_SERIAL_MAX_LINE == 63, "max line must be 63 chars + NUL");
_Static_assert(DASH_CH_COUNT == 25, "serial protocol names exactly 25 channels");
_Static_assert(DASH_CMD_NONE == 0, "DASH_CMD_NONE must be the zero value");
_Static_assert(DASH_ERR_NONE == 0, "DASH_ERR_NONE must be the zero value");
/* protocol alarm codes stay numerically identical to dash_math.h's DashAlarm
 * enum even though the macros are now DASH_SERIAL_-prefixed to avoid the
 * name collision */
_Static_assert(DASH_SERIAL_ALARM_OFF == 0, "alarm off must stay wire code 0");
_Static_assert(DASH_SERIAL_ALARM_OILP == 1, "alarm oilp must stay wire code 1");
_Static_assert(DASH_SERIAL_ALARM_OILT == 2, "alarm oilt must stay wire code 2");
_Static_assert(DASH_SERIAL_ALARM_CLT == 3, "alarm clt must stay wire code 3");

static int failures = 0;

static void expect(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

static int nearf(float a, float b, float eps)
{
    float d = a - b;
    if (d < 0.0f) { d = -d; }
    return d <= eps;
}

/* parse helper: returns the error, leaves the command in *out */
static DashSerialErr parse(const char *line, DashCommand *out)
{
    memset(out, 0xEE, sizeof *out); /* poison so untouched fields are obvious */
    return dash_parse_line(line, out);
}

int main(void)
{
    DashCommand c;

    /* ---- happy-path parses ---- */
    expect(parse("set rpm 3500", &c) == DASH_ERR_NONE, "set rpm 3500 must parse");
    expect(c.kind == DASH_CMD_SET, "set rpm 3500 must be DASH_CMD_SET");
    expect(c.channel == DASH_CH_RPM, "set rpm 3500 must target DASH_CH_RPM");
    expect(nearf(c.value, 3500.0f, 1e-3f), "set rpm 3500 must carry value 3500");

    expect(parse("set oilp 25.5", &c) == DASH_ERR_NONE, "set oilp 25.5 must parse");
    expect(c.kind == DASH_CMD_SET && c.channel == DASH_CH_OILP,
           "set oilp must target DASH_CH_OILP");
    expect(nearf(c.value, 25.5f, 1e-4f), "set oilp 25.5 must carry value 25.5");

    expect(parse("mode street", &c) == DASH_ERR_NONE, "mode street must parse");
    expect(c.kind == DASH_CMD_MODE && c.mode == DASH_MODE_STREET,
           "mode street must select DASH_MODE_STREET");
    expect(parse("mode track", &c) == DASH_ERR_NONE, "mode track must parse");
    expect(c.kind == DASH_CMD_MODE && c.mode == DASH_MODE_TRACK,
           "mode track must select DASH_MODE_TRACK");

    expect(parse("alarm oilt", &c) == DASH_ERR_NONE, "alarm oilt must parse");
    expect(c.kind == DASH_CMD_ALARM && c.alarm == 2, "alarm oilt must be alarm code 2");
    expect(parse("alarm oilp", &c) == DASH_ERR_NONE && c.alarm == 1,
           "alarm oilp must be alarm code 1");
    expect(parse("alarm clt", &c) == DASH_ERR_NONE && c.alarm == 3,
           "alarm clt must be alarm code 3");
    expect(parse("alarm off", &c) == DASH_ERR_NONE, "alarm off must parse");
    expect(c.kind == DASH_CMD_ALARM && c.alarm == 0, "alarm off must be alarm code 0");

    expect(parse("odo set 24318.2", &c) == DASH_ERR_NONE, "odo set 24318.2 must parse");
    expect(c.kind == DASH_CMD_ODO_SET, "odo set must be DASH_CMD_ODO_SET");
    expect(nearf(c.value, 24318.2f, 1e-1f), "odo set must carry the miles value");

    expect(parse("sim on", &c) == DASH_ERR_NONE, "sim on must parse");
    expect(c.kind == DASH_CMD_SIM && c.sim_on, "sim on must set sim_on");
    expect(parse("sim off", &c) == DASH_ERR_NONE, "sim off must parse");
    expect(c.kind == DASH_CMD_SIM && !c.sim_on, "sim off must clear sim_on");

    expect(parse("clear ect", &c) == DASH_ERR_NONE, "clear ect must parse");
    expect(c.kind == DASH_CMD_CLEAR && c.channel == DASH_CH_ECT,
           "clear ect must be CLEAR on DASH_CH_ECT");

    expect(parse("status", &c) == DASH_ERR_NONE && c.kind == DASH_CMD_STATUS,
           "status must be DASH_CMD_STATUS");
    expect(parse("help", &c) == DASH_ERR_NONE && c.kind == DASH_CMD_HELP,
           "help must be DASH_CMD_HELP");

    /* ---- every channel name maps to its id (25 names, incl. last) ---- */
    {
        static const struct { const char *name; uint8_t ch; } map[] = {
            { "rpm", DASH_CH_RPM },       { "speed", DASH_CH_SPEED },
            { "ect", DASH_CH_ECT },       { "oilt", DASH_CH_OILT },
            { "oilp", DASH_CH_OILP },     { "volts", DASH_CH_VOLTS },
            { "fuel", DASH_CH_FUEL },     { "delta", DASH_CH_DELTA },
            { "lap", DASH_CH_LAP },       { "last", DASH_CH_LAST },
            { "best", DASH_CH_BEST },     { "ambient", DASH_CH_AMBIENT },
            { "afr_l", DASH_CH_AFR_L },   { "afr_r", DASH_CH_AFR_R },
            { "iat", DASH_CH_IAT },       { "fuelp", DASH_CH_FUELP },
            { "throttle", DASH_CH_THROTTLE }, { "brake", DASH_CH_BRAKE },
            { "lapn", DASH_CH_LAPN },     { "pos", DASH_CH_POS },
            { "pred", DASH_CH_PRED },     { "time", DASH_CH_TIME },
            { "pump", DASH_CH_PUMP },     { "fan1", DASH_CH_FAN1 },
            { "fan2", DASH_CH_FAN2 },
        };
        for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        {
            char line[32];
            snprintf(line, sizeof line, "clear %s", map[i].name);
            expect(parse(line, &c) == DASH_ERR_NONE, "every channel name must parse");
            expect(c.channel == map[i].ch, "channel name must map to its DASH_CH_ id");
            expect(strcmp(dash_ch_name(map[i].ch), map[i].name) == 0,
                   "dash_ch_name must round-trip the protocol name");
        }
    }

    /* ---- case / whitespace tolerance: all three parse identically ---- */
    {
        const char *variants[] = { "SET RPM 3500", "set   rpm   3500", "set rpm 3500\r" };
        for (unsigned i = 0; i < 3; i++)
        {
            expect(parse(variants[i], &c) == DASH_ERR_NONE, "variant must parse clean");
            expect(c.kind == DASH_CMD_SET && c.channel == DASH_CH_RPM &&
                       nearf(c.value, 3500.0f, 1e-3f),
                   "case/space/CR variant must parse identically to set rpm 3500");
        }
    }

    /* ---- errors, each with its distinct code ---- */
    expect(parse("set bogus 5", &c) == DASH_ERR_UNKNOWN_CHANNEL,
           "set bogus must be UNKNOWN_CHANNEL");
    expect(parse("set rpm", &c) == DASH_ERR_MISSING_VALUE,
           "set rpm (no value) must be MISSING_VALUE");
    expect(parse("set rpm abc", &c) == DASH_ERR_BAD_VALUE,
           "set rpm abc must be BAD_VALUE");
    expect(parse("set rpm -5", &c) == DASH_ERR_RANGE, "set rpm -5 must be RANGE");
    expect(parse("set rpm 12001", &c) == DASH_ERR_RANGE, "set rpm 12001 must be RANGE");
    expect(parse("set rpm 12000", &c) == DASH_ERR_NONE, "set rpm 12000 must be in range");
    expect(parse("set delta -30", &c) == DASH_ERR_NONE, "set delta -30 must be in range");
    expect(parse("set delta -30.5", &c) == DASH_ERR_RANGE, "set delta -30.5 must be RANGE");
    expect(parse("set best 3600000", &c) == DASH_ERR_NONE, "set best 3600000 must be in range");
    expect(parse("set best 3600001", &c) == DASH_ERR_RANGE, "set best 3600001 must be RANGE");

    /* ---- range rejection: each new channel's bounds, above and below (KTD5) ---- */
    {
        static const struct { const char *name; float lo; float hi; float eps; } ranges[] = {
            { "afr_l", 8.0f, 20.0f, 0.1f },
            { "afr_r", 8.0f, 20.0f, 0.1f },
            { "iat", -20.0f, 300.0f, 0.1f },
            { "fuelp", 0.0f, 100.0f, 0.1f },
            { "throttle", 0.0f, 100.0f, 0.1f },
            { "brake", 0.0f, 100.0f, 0.1f },
            { "lapn", 0.0f, 999.0f, 1.0f },
            { "pos", 1.0f, 99.0f, 1.0f },
            { "pred", 0.0f, 599999.0f, 1.0f },
            { "time", 0.0f, 1439.0f, 1.0f },
            { "pump", 0.0f, 30.0f, 0.1f },
            { "fan1", 0.0f, 30.0f, 0.1f },
            { "fan2", 0.0f, 30.0f, 0.1f },
        };
        for (unsigned i = 0; i < sizeof ranges / sizeof ranges[0]; i++)
        {
            char line[48];
            snprintf(line, sizeof line, "set %s %g", ranges[i].name, (double) ranges[i].lo);
            expect(parse(line, &c) == DASH_ERR_NONE, "new channel: lo bound must be in range");
            snprintf(line, sizeof line, "set %s %g", ranges[i].name, (double) ranges[i].hi);
            expect(parse(line, &c) == DASH_ERR_NONE, "new channel: hi bound must be in range");
            snprintf(line, sizeof line, "set %s %g", ranges[i].name, (double) (ranges[i].lo - ranges[i].eps));
            expect(parse(line, &c) == DASH_ERR_RANGE, "new channel: below lo bound must be RANGE");
            snprintf(line, sizeof line, "set %s %g", ranges[i].name, (double) (ranges[i].hi + ranges[i].eps));
            expect(parse(line, &c) == DASH_ERR_RANGE, "new channel: above hi bound must be RANGE");
        }
    }

    expect(parse("frobnicate 1", &c) == DASH_ERR_UNKNOWN_CMD,
           "frobnicate must be UNKNOWN_CMD");
    expect(parse("", &c) == DASH_ERR_EMPTY, "empty line must be EMPTY");
    expect(parse("   ", &c) == DASH_ERR_EMPTY, "whitespace-only line must be EMPTY");
    expect(parse("mode sideways", &c) == DASH_ERR_BAD_VALUE,
           "mode sideways must be BAD_VALUE");
    expect(parse("odo bogus 1", &c) == DASH_ERR_BAD_VALUE,
           "odo bogus must be BAD_VALUE (only 'odo set' exists)");
    expect(parse("odo set -1", &c) == DASH_ERR_RANGE, "odo set -1 must be RANGE");

    /* 70-char line -> TOO_LONG */
    {
        char line[80];
        memset(line, 'a', 70);
        line[70] = '\0';
        expect(parse(line, &c) == DASH_ERR_TOO_LONG, "70-char line must be TOO_LONG");
    }

    /* 63-char junk line is length-legal: must return an error, not crash */
    {
        char line[80];
        memset(line, 'x', DASH_SERIAL_MAX_LINE);
        line[DASH_SERIAL_MAX_LINE] = '\0';
        expect(parse(line, &c) == DASH_ERR_UNKNOWN_CMD,
               "63-char junk line must parse to UNKNOWN_CMD, not overrun");
    }

    /* ---- application: sticky override / clear semantics ---- */
    {
        DashState s;
        char reply[64];
        dash_state_init(&s);

        /* SET rpm 3500: value written, valid + overridden, cleared dropped */
        expect(parse("set rpm 3500", &c) == DASH_ERR_NONE, "apply: set rpm parses");
        expect(dash_apply_command(&s, &c, reply, sizeof reply),
               "apply must handle SET");
        expect(dash_ch_valid(&s, DASH_CH_RPM), "SET must mark rpm valid");
        expect((s.overridden & DASH_CH_BIT(DASH_CH_RPM)) != 0, "SET must mark rpm overridden");
        expect(nearf(s.ch.rpm, 3500.0f, 1e-3f), "SET must write rpm value 3500");
        expect(strcmp(reply, "ok set rpm 3500") == 0, "SET reply must be 'ok set rpm 3500'");

        /* CLEAR ect: invalid + cleared, override dropped */
        expect(parse("clear ect", &c) == DASH_ERR_NONE, "apply: clear ect parses");
        expect(dash_apply_command(&s, &c, reply, sizeof reply), "apply must handle CLEAR");
        expect(!dash_ch_valid(&s, DASH_CH_ECT), "CLEAR must drop the valid bit");
        expect((s.cleared & DASH_CH_BIT(DASH_CH_ECT)) != 0, "CLEAR must set the cleared bit");
        expect((s.overridden & DASH_CH_BIT(DASH_CH_ECT)) == 0,
               "CLEAR must drop any override bit");
        expect(strcmp(reply, "ok clear ect") == 0, "CLEAR reply must be 'ok clear ect'");

        /* SET then CLEAR the same channel: invalid, override dropped */
        expect(parse("set speed 88", &c) == DASH_ERR_NONE, "apply: set speed parses");
        dash_apply_command(&s, &c, reply, sizeof reply);
        expect(parse("clear speed", &c) == DASH_ERR_NONE, "apply: clear speed parses");
        dash_apply_command(&s, &c, reply, sizeof reply);
        expect(!dash_ch_valid(&s, DASH_CH_SPEED), "SET-then-CLEAR must end invalid");
        expect((s.overridden & DASH_CH_BIT(DASH_CH_SPEED)) == 0,
               "SET-then-CLEAR must drop the override");
        expect((s.cleared & DASH_CH_BIT(DASH_CH_SPEED)) != 0,
               "SET-then-CLEAR must leave the cleared bit sticky");

        /* MODE street */
        expect(parse("mode street", &c) == DASH_ERR_NONE, "apply: mode street parses");
        expect(dash_apply_command(&s, &c, reply, sizeof reply), "apply must handle MODE");
        expect(s.mode == DASH_MODE_STREET, "MODE must switch DashState.mode");
        expect(strcmp(reply, "ok mode street") == 0, "MODE reply must be 'ok mode street'");

        /* ALARM oilp: like set oilp 20 (valid + overridden) */
        expect(parse("alarm oilp", &c) == DASH_ERR_NONE, "apply: alarm oilp parses");
        expect(dash_apply_command(&s, &c, reply, sizeof reply), "apply must handle ALARM");
        expect(dash_ch_valid(&s, DASH_CH_OILP), "ALARM oilp must mark oilp valid");
        expect((s.overridden & DASH_CH_BIT(DASH_CH_OILP)) != 0,
               "ALARM oilp must override oilp");
        expect(nearf(s.ch.oil_press_psi, 20.0f, 1e-4f), "ALARM oilp must force oilp to 20");

        /* ALARM off: the three alarm channels return to the sim */
        expect(parse("alarm off", &c) == DASH_ERR_NONE, "apply: alarm off parses");
        expect(dash_apply_command(&s, &c, reply, sizeof reply),
               "apply must handle ALARM off");
        expect((s.overridden & DASH_CH_BIT(DASH_CH_OILP)) == 0,
               "ALARM off must drop the oilp override");
        expect((s.cleared & DASH_CH_BIT(DASH_CH_ECT)) == 0,
               "ALARM off must drop the ect cleared bit (sim reclaims it)");

        /* SIM off freezes; SIM on wipes both masks and unfreezes */
        expect(parse("sim off", &c) == DASH_ERR_NONE, "apply: sim off parses");
        expect(dash_apply_command(&s, &c, reply, sizeof reply), "apply must handle SIM off");
        expect(s.sim_frozen, "SIM off must freeze the sim");
        expect(strcmp(reply, "ok sim off") == 0, "SIM off reply must be 'ok sim off'");

        expect(parse("sim on", &c) == DASH_ERR_NONE, "apply: sim on parses");
        expect(dash_apply_command(&s, &c, reply, sizeof reply), "apply must handle SIM on");
        expect(s.overridden == 0, "SIM on must wipe the override mask");
        expect(s.cleared == 0, "SIM on must wipe the cleared mask");
        expect(!s.sim_frozen, "SIM on must unfreeze the sim");
        expect(strcmp(reply, "ok sim on") == 0, "SIM on reply must be 'ok sim on'");

        /* ODO_SET / STATUS / HELP are the caller's job: apply returns false */
        expect(parse("odo set 100", &c) == DASH_ERR_NONE, "apply: odo set parses");
        expect(!dash_apply_command(&s, &c, reply, sizeof reply),
               "apply must NOT handle ODO_SET");
        expect(parse("status", &c) == DASH_ERR_NONE, "apply: status parses");
        expect(!dash_apply_command(&s, &c, reply, sizeof reply),
               "apply must NOT handle STATUS");
        expect(parse("help", &c) == DASH_ERR_NONE, "apply: help parses");
        expect(!dash_apply_command(&s, &c, reply, sizeof reply),
               "apply must NOT handle HELP");
    }

    /* ---- ALARM off with the sim frozen must not latch the takeover:
     * the forced values may stay in memory, but the valid bits must drop
     * so dash_alarm_classify() stops firing ('--' beats a stale 20 psi) ---- */
    {
        DashState s;
        char reply[64];
        dash_state_init(&s);
        s.sim_frozen = true; /* as after `sim off` */

        expect(parse("alarm oilp", &c) == DASH_ERR_NONE, "frozen: alarm oilp parses");
        expect(dash_apply_command(&s, &c, reply, sizeof reply),
               "frozen: apply must handle ALARM oilp");
        expect(dash_ch_valid(&s, DASH_CH_OILP), "frozen: ALARM oilp must mark oilp valid");
        expect((s.overridden & DASH_CH_BIT(DASH_CH_OILP)) != 0,
               "frozen: ALARM oilp must override oilp");
        expect(nearf(s.ch.oil_press_psi, 20.0f, 1e-4f),
               "frozen: ALARM oilp must force oilp to 20");

        expect(parse("alarm off", &c) == DASH_ERR_NONE, "frozen: alarm off parses");
        expect(dash_apply_command(&s, &c, reply, sizeof reply),
               "frozen: apply must handle ALARM off");
        expect(!dash_ch_valid(&s, DASH_CH_OILP),
               "frozen: ALARM off must drop the oilp valid bit (no latched takeover)");
        expect(!dash_ch_valid(&s, DASH_CH_OILT),
               "frozen: ALARM off must drop the oilt valid bit");
        expect(!dash_ch_valid(&s, DASH_CH_ECT),
               "frozen: ALARM off must drop the ect valid bit");
        expect((s.overridden & (DASH_CH_BIT(DASH_CH_OILP) | DASH_CH_BIT(DASH_CH_OILT) |
                                DASH_CH_BIT(DASH_CH_ECT))) == 0,
               "frozen: ALARM off must drop all three override bits");

        /* a later SET revalidates the channel */
        expect(parse("set oilp 45", &c) == DASH_ERR_NONE, "frozen: set oilp 45 parses");
        expect(dash_apply_command(&s, &c, reply, sizeof reply),
               "frozen: apply must handle SET after alarm off");
        expect(dash_ch_valid(&s, DASH_CH_OILP),
               "frozen: SET after ALARM off must revalidate oilp");
        expect(nearf(s.ch.oil_press_psi, 45.0f, 1e-4f),
               "frozen: SET after ALARM off must write the new value");
    }

    /* ---- reply truncation: a tiny reply buffer must not overrun ---- */
    {
        DashState s;
        struct
        {
            char reply[8];
            char canary;
        } g;
        dash_state_init(&s);
        g.canary = 0x5A;
        expect(parse("set rpm 3500", &c) == DASH_ERR_NONE, "canary: set rpm parses");
        expect(dash_apply_command(&s, &c, g.reply, sizeof g.reply),
               "canary: apply must still handle SET with a tiny buffer");
        expect(g.canary == 0x5A, "reply must truncate inside reply_len (canary intact)");
        expect(strlen(g.reply) < sizeof g.reply, "truncated reply must stay NUL-terminated");
    }

    /* ---- AE5 (host half): set/clear afr_l round trip ---- */
    {
        DashState s;
        char reply[64];
        dash_state_init(&s);

        expect(parse("set afr_l 12.8", &c) == DASH_ERR_NONE, "AE5: set afr_l 12.8 parses");
        expect(dash_apply_command(&s, &c, reply, sizeof reply), "AE5: apply must handle SET afr_l");
        expect(dash_ch_valid(&s, DASH_CH_AFR_L), "AE5: SET afr_l must mark it valid");
        expect(nearf(dash_ch_get(&s, DASH_CH_AFR_L), 12.8f, 1e-4f), "AE5: afr_l must read back 12.8");
        expect(strcmp(reply, "ok set afr_l 12.8") == 0, "AE5: reply must be 'ok set afr_l 12.8'");

        expect(parse("clear afr_l", &c) == DASH_ERR_NONE, "AE5: clear afr_l parses");
        expect(dash_apply_command(&s, &c, reply, sizeof reply), "AE5: apply must handle CLEAR afr_l");
        expect(!dash_ch_valid(&s, DASH_CH_AFR_L), "AE5: clear afr_l must invalidate it");
        expect((s.cleared & DASH_CH_BIT(DASH_CH_AFR_L)) != 0, "AE5: clear afr_l must set the cleared bit");
    }

    /* ---- AE3: flashwipe guard -- the bare verb must never reach the
     * wipe handler; only the literal confirmation argument parses ---- */
    {
        DashState s;
        char reply[64];
        dash_state_init(&s);

        expect(parse("flashwipe", &c) == DASH_ERR_MISSING_VALUE,
               "AE3: bare flashwipe must be MISSING_VALUE (no erase)");
        expect(c.kind == DASH_CMD_NONE, "AE3: bare flashwipe must not set a command");
        expect(parse("flashwipe yes", &c) == DASH_ERR_BAD_VALUE,
               "AE3: flashwipe with wrong argument must be BAD_VALUE");
        expect(parse("flashwipe really", &c) == DASH_ERR_NONE,
               "AE3: flashwipe really must parse");
        expect(c.kind == DASH_CMD_FLASHWIPE,
               "AE3: flashwipe really must be DASH_CMD_FLASHWIPE");
        expect(parse("FLASHWIPE REALLY", &c) == DASH_ERR_NONE &&
                   c.kind == DASH_CMD_FLASHWIPE,
               "AE3: flashwipe must be case-insensitive like the protocol");
        expect(!dash_apply_command(&s, &c, reply, sizeof reply),
               "AE3: apply must NOT handle FLASHWIPE (EVE-bound, caller executes)");
    }

    /* help text exists and mentions every command verb and new channels */
    expect(strstr(DASH_HELP_TEXT, "set") != NULL &&
               strstr(DASH_HELP_TEXT, "clear") != NULL &&
               strstr(DASH_HELP_TEXT, "mode") != NULL &&
               strstr(DASH_HELP_TEXT, "alarm") != NULL &&
               strstr(DASH_HELP_TEXT, "odo") != NULL &&
               strstr(DASH_HELP_TEXT, "sim") != NULL &&
               strstr(DASH_HELP_TEXT, "status") != NULL &&
               strstr(DASH_HELP_TEXT, "flashwipe") != NULL,
           "DASH_HELP_TEXT must list every command verb");
    expect(strstr(DASH_HELP_TEXT, "afr_l") != NULL &&
               strstr(DASH_HELP_TEXT, "afr_r") != NULL &&
               strstr(DASH_HELP_TEXT, "iat") != NULL &&
               strstr(DASH_HELP_TEXT, "fuelp") != NULL &&
               strstr(DASH_HELP_TEXT, "throttle") != NULL &&
               strstr(DASH_HELP_TEXT, "brake") != NULL &&
               strstr(DASH_HELP_TEXT, "lapn") != NULL &&
               strstr(DASH_HELP_TEXT, "pos") != NULL &&
               strstr(DASH_HELP_TEXT, "pred") != NULL &&
               strstr(DASH_HELP_TEXT, "time") != NULL &&
               strstr(DASH_HELP_TEXT, "pump") != NULL &&
               strstr(DASH_HELP_TEXT, "fan1") != NULL &&
               strstr(DASH_HELP_TEXT, "fan2") != NULL,
           "DASH_HELP_TEXT must list every new Phase-2 channel");

    if (failures == 0)
    {
        printf("OK: dash serial protocol matches the plan (parse, ranges, errors, sticky apply)\n");
        return 0;
    }
    return 1;
}
