/* Packages for systems with an older glibc (docs/adr/0046). glibc 2.43 gave acosf and sqrtf new symbol
   versions; the programs of a package are linked with --wrap=acosf,--wrap=sqrtf and call the versions
   every x86-64 glibc has, so a package needs glibc 2.38, not the glibc of the machine it was built on. */
__asm__(".symver kk_acosf_compat, acosf@GLIBC_2.2.5");
__asm__(".symver kk_sqrtf_compat, sqrtf@GLIBC_2.2.5");
float kk_acosf_compat(float);
float kk_sqrtf_compat(float);
float __wrap_acosf(float x) { return kk_acosf_compat(x); }
float __wrap_sqrtf(float x) { return kk_sqrtf_compat(x); }
