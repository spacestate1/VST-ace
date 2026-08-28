/* A minimal Objective-C runtime for Mach-O plugins. See macobjc.c for why the
 * Apple classes have to be ours regardless of what runtime is used. */
#ifndef PELOAD_MACOBJC_H
#define PELOAD_MACOBJC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The assembly trampolines (macobjc_msgsend.S). Declared as data-free function
 * symbols: their real signatures are variadic and register-exact, so nothing
 * should ever call them from C. */
void macobjc_msgSend(void);
void macobjc_nil_return(void);
void macobjc_msgSend_stret(void);
void macobjc_msgSendSuper2(void);
void macobjc_msgSendSuper2_stret(void);
/* The legacy fixup dispatch ABI: the selector comes from a per-call-site
 * message_ref rather than from %rsi. See macobjc_msgsend.S. */
void macobjc_msgSend_fixup(void);
void macobjc_msgSend_stret_fixup(void);
void macobjc_msgSendSuper2_fixup(void);

/* Called by the trampolines. */
void (*macobjc_lookup(void *self, const char *sel))(void);
void (*macobjc_lookup_super(void *super2, const char *sel))(void);
void *macobjc_super_receiver(void *super2);

/* Resolve _objc_* and _OBJC_CLASS_$_* / _OBJC_METACLASS_$_* names. */
void *macobjc_lookup_symbol(const char *sym);

/* A class object by name, or NULL. */
void *macobjc_class(const char *name);

/* Create a stand-in class if it does not exist yet, and return it. Needed for
 * Metal, whose objects are typed by protocol -- there is no _OBJC_CLASS_$_ for
 * id<MTLDevice>, so nothing in the image causes the class to be minted. */
void *macobjc_define_class(const char *name);

/* Attach a real implementation to a stand-in class. A '+' prefix on `sel` makes
 * it a class method. The implementation takes (id self, const char *sel, ...). */
int   macobjc_add_method(const char *cls, const char *sel, void *imp);

/* True when obj's class or an ancestor is named `name`. */
int   macobjc_isa_named(void *obj, const char *name);

/* Note the image's own classes so a message to one can be traced. */
void  macobjc_register_image_classes(void *const *classlist, size_t count);

/* How many objects have been retired (released to zero). They are not freed --
 * see the note in macobjc.c -- so this is also the count of objects leaked. */
unsigned long macobjc_retired_count(void);

/* Selectors that were sent to something with no implementation -- the work list
 * for making a GUI actually run. */
int   macobjc_missed_count(void);
void  macobjc_each_missed(void (*cb)(const char *cls, const char *sel,
                                     unsigned long n, void *ud), void *ud);

#ifdef __cplusplus
}
#endif

#endif /* PELOAD_MACOBJC_H */
