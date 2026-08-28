/* Check macvdsp's vDSP_fft_zrip against a reference, in Apple's packing and
 * scaling convention. Prints the spectrum so a script can diff it against numpy,
 * and reports the error of a forward/inverse round trip. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../peload/macshim.h"

int main(int argc, char **argv)
{
    unsigned long log2n = argc > 1 ? strtoul(argv[1], NULL, 10) : 6;
    unsigned long n = 1ul << log2n, h = n / 2, i;
    float *x = malloc(n * sizeof *x), *re = malloc(h * sizeof *re),
          *im = malloc(h * sizeof *im), *back = malloc(n * sizeof *back);
    void *setup = macshim_fftsetup(log2n);
    double worst = 0.0;

    /* a signal with content in several bins, nothing symmetric */
    for (i = 0; i < n; i++)
        x[i] = (float)(sin(2.0 * M_PI * 3.0 * i / n) * 1.0 +
                       cos(2.0 * M_PI * 7.0 * i / n) * 0.5 +
                       sin(2.0 * M_PI * 11.0 * i / n + 0.4) * 0.25);

    /* interleaved even/odd into split complex, as vDSP_ctoz does */
    for (i = 0; i < h; i++) { re[i] = x[2 * i]; im[i] = x[2 * i + 1]; }
    macshim_fft_zrip(setup, re, im, log2n, 1);

    printf("N %lu\n", n);
    for (i = 0; i < h; i++) printf("%.6f %.6f\n", re[i], im[i]);

    /* round trip */
    macshim_fft_zrip(setup, re, im, log2n, -1);
    for (i = 0; i < h; i++) { back[2 * i] = re[i]; back[2 * i + 1] = im[i]; }
    for (i = 0; i < n; i++) {
        double got = back[i] / (2.0 * (double)n);   /* Apple's documented 1/(2n) */
        double e = fabs(got - x[i]);
        if (e > worst) worst = e;
    }
    fprintf(stderr, "round-trip max error: %.3e\n", worst);
    return worst < 1e-4 ? 0 : 1;
}
