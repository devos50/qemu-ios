/* Guest-side stack readiness check, built with contrib/armv6-toolchain.
 * Firmware-download traces alone miss BluetoothManager client timeouts.
 * Dynamic lookup keeps this compatible with the iPhoneOS 3.1.3 SDK.
 */
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int fflush(void *);
extern int printf(const char *, ...);
extern void _exit(int);
struct timeval { long tv_sec; int tv_usec; };
extern int gettimeofday(struct timeval *, void *);
static double clock_ms(void) {
    struct timeval t;
    gettimeofday(&t, 0);
    return t.tv_sec * 1000.0 + t.tv_usec / 1000.0;
}
int main(void) {
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", 2);
    void *(*class_named)(const char *) = dlsym(objc, "objc_getClass");
    void *(*selector)(const char *) = dlsym(objc, "sel_registerName");
    void *(*send)(void *, void *) = dlsym(objc, "objc_msgSend");
    if (!class_named || !selector || !send) { return 1; }
    dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", 2);
    dlopen("/System/Library/PrivateFrameworks/BluetoothManager.framework/BluetoothManager", 2);
    void *pool = send(send(class_named("NSAutoreleasePool"), selector("alloc")), selector("init"));
    double start = clock_ms();
    void *manager = send(class_named("BluetoothManager"), selector("sharedInstance"));
    double duration = clock_ms() - start;
    printf("sharedInstance=%p %.1f ms\n", manager, duration);
    int failed = !manager || duration >= 500;
    if (manager) {
        const char *names[] = {"enabled", "powered"};
        long (*query)(void *, void *) = (void *)send;
        for (int i = 0; i < 2; i++) {
            start = clock_ms();
            long value = query(manager, selector(names[i]));
            duration = clock_ms() - start;
            printf("%s=%ld %.1f ms\n", names[i], value, duration);
            failed |= duration >= 500;
        }
    }
    send(pool, selector("drain"));
    fflush(0);
    _exit(failed);
}
