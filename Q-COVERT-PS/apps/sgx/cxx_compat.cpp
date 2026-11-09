// Intel SGX SDK 2.26's libsgx_tcxx.a references the ELF DSO finalizer while
// the trusted runtime owns enclave destructor dispatch through do_atexit_aux.
// Newer host toolchains diagnose the otherwise unresolved compatibility hook.
extern "C" void __cxa_finalize(void*) noexcept {}
