TriCore Bitfield Packing Test
#############################

Demonstrates that the TriCore clang toolchain packs uint32_t bitfield
structs to minimum byte size (sizeof=1 for 6 bits) instead of
honoring the uint32_t backing type (sizeof=4). Struct assignment
writes only 1 byte, leaving upper bytes stale.

Build and run::

   west build -b kit_a3g_tc4d7_lite/tc4d7xp/tc samples/boards/infineon/tricore_bitfield_pack

Expected output::

   sizeof no_pad.flags = 1 (expected 4)
   sizeof padded.flags = 4 (expected 4)
   no_pad:  canary=0xAAAAAAAA (expect 0xAAAAAAAA)
   padded:  canary=0xAAAAAAAA (expect 0xAAAAAAAA)

The sizeof=1 for no_pad.flags confirms the toolchain bug. While
the canary may survive in this simple test, sub-word access causes
real failures when these structs are embedded in larger objects
accessed concurrently or via volatile pointers.
