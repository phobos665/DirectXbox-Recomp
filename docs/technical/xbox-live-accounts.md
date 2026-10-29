# Xbox Live accounts without a dashboard

**Goal:** a player with no Xbox makes their Insignia account (a gamertag, with
the subscription code Insignia emails them) from the launcher, and the account
is written onto the game's disk, so no dashboard is needed. Registering the
console is done (`docs/technical/xbox-live.md`, "With no backup"); this is the
step after it.

**Status (29 Oct 2026):** the protocol's shape is read out of the 5960 online
dashboard (`xodash/xonlinedash.xbe`, from a console's C: drive; studied only,
never shipped). The core requests are mapped (below). Nothing has been sent.

## What the dashboard does

Making an account is a run of HTTP POSTs to **service 5, User Account
Creation** (`xuacs`), sent through the security gateway like the rest of a
signed-in console's traffic. Xombie's gateway lists service 5 by that name
and forwards it without implementing it, so the server side is Insignia's
alone.

The client is the Xbox Live library (the `XONLINE` section), not the
dashboard's own code. A table in `XON_RD` holds one 20-byte entry per
request, `{service, ?, path, ?, ?}`; a request's builder hands its entry to
`sub_12CF6D`, which sets up the request.

| Request | Service | Built in | What it is for |
| --- | --- | --- | --- |
| `/xuacs/reservename.ashx` | 5 | near `0x121EE1` (uses the path directly) | hold a gamertag |
| `/xuacs/verifyvouchergetoffer.ashx` | 5 | `sub_121B61` | check the subscription code, get its offer |
| `/xuacs/verifyvoucher.ashx` | 5 | `sub_1215F8` | check a code |
| `/xuacs/verifybillingpin.ashx` | 5 | `sub_1215F8` | |
| `/xuacs/accepttou.ashx` | 5 | `sub_12180E` | accept the terms of use |
| `/xuacs/troubleshootaccount.ashx` | 5 | `sub_12180E` | |
| `/xuacs/getpostalcode.ashx` | 5 | near `0x122C26` (uses the path directly) | |
| `/xuacs/getpaymentinfo.ashx` | 5 | `sub_120D5D` | |
| `/xuacs/setpaymentinfo.ashx` | 5 | `sub_121237` | the card (Insignia: a test number) |
| `/xuacs/setusersettings.ashx` | 5 | `sub_121237` | |
| `/xuacs/createowneracct.ashx` | 5 | `sub_12248A` | create the account |
| `/xuacs/setuserpin.ashx` | 5 | `sub_122746` | the account's PIN |
| `/xuacs/restoreaccount.ashx` | 5 | `sub_122746` | recover an account onto a console |
| `/xuacs/changegamertag.ashx` | 5 | `sub_1229E6` | |
| `/xuacs/getsubscriptionstatus.ashx` | 5 | `sub_122214` | |
| `/xuacs/EnumerateEligible.ashx` | 5 | `sub_122214` | |
| `/xbos/OfferingDetails*.ashx`, `OfferingCancel.ashx` | | near `0x11A534`-`0x11A807` | the billing/offer service; possibly title updates' route |

Unreferenced in this build: `getlockoutinfo`, `createuseracct`,
`setuseraccountstatus`, `clearsubscriptions`, `/xbos/verifynickname`.

Addresses are this build's (5960 `xonlinedash.xbe`).

## How a body is written

Binary, little-endian, built field by field into the request buffer:

- fixed-size numbers and bytes at fixed places;
- text fields one after another, each by `sub_120ADF`: the field is
  converted from UTF-16 to **UTF-8** (`WideCharToMultiByte`, code page
  65001), preceded by its length as a **16-bit** count; an empty field is
  just a zero length.

Every request goes out through `sub_1152B4` (service, path, reply buffer and
size, body and size, a timeout of 180 000 ms), directly or by way of the
request object `sub_12CF6D` sets up. Most bodies start with the console's
**machine ID**, 8 bytes the library keeps at `+0x24` of its state
(`sub_10ED3B`): the machine account's ID.

## The requests, field by field

Offsets are into the body. "Text" is a 16-bit length and UTF-8. What a field
*means* is marked *(to confirm)* where only its place is known; the live
capture settles those.

**`reservename`** (near `0x121DBC`): hold a gamertag, and get alternatives.

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 8 | machine ID |
| 0x08 | 1 | a flag from the caller *(to confirm)* |
| 0x09 | 2 | how many alternative names to send back |
| 0x0B | text | the gamertag |

Reply buffer: `count * 20 + 0x202` bytes, room for the alternatives.

**`verifyvouchergetoffer`** (`sub_121B61`): check the subscription code.

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 1 | a byte from the caller: the country *(to confirm)* |
| 0x01 | text | the subscription code |

Sent as a fixed 0x4E bytes; reply buffer 0x208. No machine ID.

**`createowneracct`** (`sub_12248A`): create the account.

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 8 | machine ID |
| 0x08 | 4 | zero |
| 0x0C | 4+4 | two dwords from the caller's `+0x24`/`+0x28` *(to confirm)* |
| 0x14 | 1, 1 | owner record `+0x8DA`, `+0x8DB` *(language, opt-in?)* |
| 0x16 | 1 | owner `+0x500` *(country?)* |
| 0x17 | 2 | from `sub_7CADD` *(a locale or time-zone word?)* |
| 0x19 | 1 | owner `+0x5EA` *(card type?)* |
| 0x1A | 4+4 | owner `+0x61C`, `+0x620` *(a FILETIME: card expiry?)* |
| 0x22 | text x 19 | the gamertag (the caller's argument), then 18 owner-record fields: `+0x0` (64 chars), `+0x80` (64), `+0x100` (128), `+0x200` (128), `+0x300` (128), `+0x400` (64), `+0x480` (64), `+0x502` (16), `+0x522` (12), `+0x53A` (12), `+0x552` (12), `+0x56A` (64), `+0x5EC` (24), `+0x624` (129), `+0x726` (64), `+0x7A6` (64), `+0x826` (64), `+0x8A6` (26) *(names, street lines, city, state, postal code, phone, card holder, card number, e-mail, bank fields?)* |

Field sizes are the widths of the owner record's text slots, in UTF-16
characters. **The reply** (0x244 bytes, read by `sub_121F40`) is the account:

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 8 | the user's ID (XUID) |
| 0x08 | 16 | the account key |
| 0x18 | 20 | Kerberos domain |
| 0x2C | 24 | Kerberos realm |

from which the library builds the stored account record (ID, gamertag at
0x0C, options and PIN from the caller, domain at 0x24, realm at 0x38, key at
0x50, the time at 0x60) and writes it to the disk (`sub_119A79`). That is the
108-byte record of the config area's user sectors, so once Insignia answers
this request the account can be written exactly as the console writes it.

**`accepttou`** (`sub_12180E`): 16 bytes, the user's ID (from the signed-in
users, `0x70` bytes each) and the machine ID. So the new account is signed in
before the terms are accepted. Reply buffer 0x200.

**`setuserpin`** (`sub_122746`): 8 bytes, the user's ID. **The PIN is not
sent**: it stays in the record (options bit 0, the passcode at 0x20). The
reply (0x210, `sub_121FFD`) carries a new 16-byte account key, which goes into
the record at 0x50 before it is written again.

## The order

From the dependencies above: sign in as the machine; `reservename`;
`verifyvouchergetoffer`; (`setpaymentinfo`, Insignia's test card); 
`createowneracct`, write the account; sign in as the new user; `accepttou`;
`setuserpin` if a PIN is wanted. To check against the live capture.

## Measured against Insignia (29 Oct 2026)

From `titles/xodash-online` with `RECOMP_XUACS_DRIVE`, on a generated
console registered through `insignia_register.c` (HTTP 200), signed in by
the dashboard's own library (MACS, AS, TGS, two security gateways):

- **The country list** is `/Europe/Country/CountryMaster2.INX` (also
  `/America/...`, `/Asia/...`), service 11, a UTF-16 INI of
  `[GB] LocaleCode="35"` entries: GB 35, US 103, DE 24, FR 34, JP 53 and 34
  more. The country byte every account request carries is that code.
- **`reservename`** for a free name: body machine ID, country `0x23`, count
  5, the name; answer `HTTP/1.1 200 OK`, `Content-Type: xon/5`,
  `Content-Length: 2`.
- **`verifyvouchergetoffer`**: answer `HTTP/1.1 200 OK` with
  **`X-Err: 00000001`** and no body. Xbox Live carries a request's result in
  `X-Err`; what 1 means here is not known yet.
- A reply buffer first receives the HTTP response's headers
  (NUL-separated); the body is then written over its start. So
  `reservename`'s reply began `00 00` followed by the rest of "HTTP/1.1":
  its 2-byte body is a count of 0 alternatives, **the name is free**.
- `X-Err` is parsed as hex into the request's web state (`+0x10`), apart
  from the request's own result (`+0x168`, `0x001500F0` here). The
  results function (`sub_121C0B`) returns that result and an 8-byte offer
  ID from the start of the body; the dashboard (`0x5FE4B`) goes on to the
  next sign-up scene (0x29) whenever the result is not a failure. So an
  `X-Err: 1` answer with no body is accepted as a console would accept it.

- **Insignia checks the code at `createowneracct`, not before.**
  `verifyvouchergetoffer` answers `X-Err: 00000001` for a made-up code
  exactly as for a real one. `createowneracct` with a code and e-mail that
  do not match answers `HTTP/1.1 500` with `X-Err: 80154007`, and the task
  fails with that HRESULT.

**Owner-record fields confirmed in the dashboard's validators:** the card
type at `+0x5EA` and card number at `+0x5EC` (`sub_5A49A`), the **e-mail**
at `+0x624` (`sub_5A8D4`, which searches for `@`). The sign-up state passes
the record as `state + 0x2C`; the gamertag is at the state's start.

The drive's file (`steps`, `gamertag`, `country`, `code`) and the log both
hold the player's code: private files, never committed.

## Plan

1. **Map each body** the account flow sends (reserve name, the code, terms,
   payment, create, PIN) and each answer, from the builders above. Offline.
2. **Watch it live:** recompile `xonlinedash.xbe` as `titles/xodash-update`
   recompiled `update.xbe`, run it on a generated console with the player's
   subscription code, and log each request and answer in the clear, before
   the gateway encrypts it. This makes a real account on Insignia.
3. **Our own client:** the launcher asks for the code and a gamertag, sends
   the same requests through the gateway, and writes the account Insignia
   returns into the disk image's account sectors (8-19).

Only the message formats are reproduced; no dashboard code is copied or
shipped. Neither hakuX nor xemu has an account client of its own: both run
the player's dashboard. [Theseus](https://github.com/MrMilenko/Theseus)
(GPL-3.0) reconstructs the offline dashboard only; it reads the accounts on
the disk (`_XOnlineGetUsersFromHD`, 128-byte records) and does not create one.
