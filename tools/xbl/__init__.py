"""tools.xbl: an Xbox Live client of our own, for Insignia.

What a console does to sign in and talk to Xbox Live services -- Kerberos
with the console's keys, the security gateway's tunnel, and HTTP requests
inside it -- made from the host, so the launcher can register a console,
make an account and fetch title updates without a dashboard
(docs/technical/xbox-live-accounts.md).

Prototype in Python; the launcher gets a C port. Microsoft's key constants
are never in this package: they are found in the player's own game image.
"""
