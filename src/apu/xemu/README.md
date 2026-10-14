# xemu's voice processor headers

Copied unchanged from xemu (`third_party/xemu`, `hw/xbox/mcpx/apu/vp/`) at
commit e72e76bd1f, the same commit `src/apu/apu_vp_xemu.c` is copied from,
under their xemu paths so the copies include each other as in xemu. LGPL-2.1
or later, as in xemu.

They are here so the APU builds without the xemu submodule, which CI does not
fetch and which is far larger than these four files. To take a later xemu,
copy `vp.c` (re-applying the three changes listed at the top of
`apu_vp_xemu.c`) and these headers from the same commit, and update both
commit ids.

`hw/xbox/mcpx/apu/apu_regs.h` here only includes the toolkit's own copy
(`src/apu/apu_regs.h`), which is identical to xemu's.
