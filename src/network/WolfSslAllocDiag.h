#pragma once

// Diagnostic-only wolfSSL allocator hook. Installs pass-through malloc/
// realloc/free callbacks via wolfSSL_SetAllocators so every wolfCrypt/wolfSSL
// allocation (including the multi-KB DH window table used during a TLS
// handshake) flows through a wrapper that logs the requested size plus
// ESP.getFreeHeap()/ESP.getMaxAllocHeap() whenever an allocation fails. This
// does not change allocation behavior (same semantics as the default
// malloc/realloc/free) — it only gives the next on-device run an exact
// measurement of which allocation is failing and at what heap headroom.
// See docs/superpowers/specs/2026-07-22-translation-kosync-teardown-fix.md
// section "Diagnostic wolfSSL allocator".
//
// Gated on FREEINK_NET_WOLFSSL: with the flag off this compiles to a no-op.

// Install the pass-through allocator wrapper. Call once during setup(),
// before any code path can trigger a TLS handshake.
void installWolfSslAllocDiag();
