# Xbox Live accounts without a dashboard

**Goal:** a player with no Xbox makes their Insignia account (a gamertag, with
the subscription code Insignia emails them) from the launcher, and the account
is written onto the game's disk, so no dashboard is needed. Registering the
console is done (`docs/technical/xbox-live.md`, "With no backup"); this is the
step after it.

**Status (29 Oct 2026):** the protocol's shape is read out of the 5960 online
dashboard (`xodash/xonlinedash.xbe`, from a console's C: drive; studied only,
never shipped). Request bodies are being mapped. Nothing has been sent.

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

`createowneracct` (`sub_12248A`): a 0x1688-byte request; copies dwords from
the caller's `+0x24`/`+0x28`, bytes and words from the owner record
(`+0x500`, `+0x5EA`, `+0x61C`, `+0x620`, `+0x8DA`, `+0x8DB`), then about twenty
text fields from the owner record (`+0x0`, `+0x80`, `+0x100`, `+0x200`,
`+0x300`, `+0x400`, `+0x480`, `+0x502`, `+0x522`, `+0x53A`, `+0x552`, `+0x56A`,
`+0x5EC`, `+0x624`, `+0x726`, `+0x7A6`, `+0x826`, `+0x8A6`), a last one from the
request itself, and an optional flag and dword from the caller's `+0x20`. Its
answer is read into a 0x244-byte buffer by `sub_121F40`. Which field is which
(names, address, email, the code) is the next thing to map.

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
