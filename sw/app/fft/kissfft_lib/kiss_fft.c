/* kiss_fft.c */
#include "_kiss_fft_guts.h"

/* Variables globales définies dans main.h, on les déclare externes ici */
extern kiss_fft_cpx g_twiddles[512];
extern int g_factors[10];

static void kf_bfly2(kiss_fft_cpx * Fout, const size_t fstride, const kiss_fft_cfg st, int m)
{
    kiss_fft_cpx * Fout2;
    kiss_fft_cpx * tw1 = st->twiddles;
    kiss_fft_cpx t;
    Fout2 = Fout + m;
    do {
        // Scaling obligatoire en int16
        C_FIXDIV(*Fout, 2);
        C_FIXDIV(*Fout2, 2);

        C_MUL (t,  *Fout2 , *tw1);
        tw1 += fstride;
        C_SUB( *Fout2 ,  *Fout , t );
        C_ADDTO( *Fout ,  t );
        ++Fout2;
        ++Fout;
    } while (--m);
}

static void kf_bfly4(kiss_fft_cpx * Fout, const size_t fstride, const kiss_fft_cfg st, const size_t m)
{
    kiss_fft_cpx *tw1,*tw2,*tw3;
    kiss_fft_cpx scratch[6];
    size_t k=m;
    const size_t m2=2*m;
    const size_t m3=3*m;

    tw3 = tw2 = tw1 = st->twiddles;

    do {
        // Scaling obligatoire en int16
        C_FIXDIV(*Fout, 4); C_FIXDIV(Fout[m], 4); C_FIXDIV(Fout[m2], 4); C_FIXDIV(Fout[m3], 4);

        C_MUL(scratch[0],Fout[m] , *tw1 );
        C_MUL(scratch[1],Fout[m2] , *tw2 );
        C_MUL(scratch[2],Fout[m3] , *tw3 );

        C_SUB( scratch[5] , *Fout, scratch[1] );
        C_ADDTO(*Fout, scratch[1]);
        C_ADD( scratch[3] , scratch[0] , scratch[2] );
        C_SUB( scratch[4] , scratch[0] , scratch[2] );
        C_SUB( Fout[m2], *Fout, scratch[3] );
        tw1 += fstride;
        tw2 += fstride*2;
        tw3 += fstride*3;
        C_ADDTO( *Fout , scratch[3] );

        if(st->inverse) {
            Fout[m].r = scratch[5].r - scratch[4].i;
            Fout[m].i = scratch[5].i + scratch[4].r;
            Fout[m3].r = scratch[5].r + scratch[4].i;
            Fout[m3].i = scratch[5].i - scratch[4].r;
        }else{
            Fout[m].r = scratch[5].r + scratch[4].i;
            Fout[m].i = scratch[5].i - scratch[4].r;
            Fout[m3].r = scratch[5].r - scratch[4].i;
            Fout[m3].i = scratch[5].i + scratch[4].r;
        }
        ++Fout;
    } while(--k);
}

/* * Fonction principale Itérative (Non-Récursive) 
 * Remplace l'ancienne kf_work récursive
 */
static void kf_work_nr(kiss_fft_cpx * Fout, const kiss_fft_cpx * f, const kiss_fft_cfg st)
{
    const int n = st->nfft;
    
    // --- Etape 1 : Permutation (Mixed Radix Bit-Reversal) ---
    // On copie l'entrée f vers la sortie Fout dans l'ordre permuté
    for (int i = 0; i < n; i++) {
        int src = 0;
        int temp_i = i;
        int weight = 1;
        
        // Lecture des facteurs depuis le tableau interne (copié depuis g_factors)
        // g_factors = {4, 128, 4, 32, ...} => p, m pairs
        const int *f_ptr = st->factors; 
        
        while (*f_ptr) {
            int p = *f_ptr++;       // Radix (ex: 4)
            int m_val = *f_ptr++;   // Stride courant (ex: 128)
            
            // Calcul de l'index inversé
            src += (temp_i / m_val) * weight;
            temp_i %= m_val; 
            weight *= p;
        }
        Fout[i] = f[src];
    }

    // --- Etape 2 : Calcul des Papillons (Butterflies) ---
    // On remonte du plus petit étage (m=1) vers le plus grand
    
    int m = 1;
    const int *fac = st->factors;
    
    // Pour N=512 avec les facteurs donnés, on a 5 étages (4,4,4,4,2)
    // On doit parcourir les facteurs à l'envers : du dernier (2) au premier (4)
    // g_factors a 10 éléments (5 paires). Le dernier 'p' est à l'index 8.
    const int *f_ptr = fac + 8; 

    while (f_ptr >= fac) {
        int p = f_ptr[0]; // Radix de l'étage courant
        
        // fstride pour les twiddles
        size_t fstride = n / (p * m);
        
        // Boucle sur tous les blocs de taille p*m
        for (int i = 0; i < n; i += p * m) {
            if (p == 4) {
                kf_bfly4(Fout + i, fstride, st, m);
            }
            else if (p == 2) {
                kf_bfly2(Fout + i, fstride, st, m);
            }
            // Ajouter d'autres cas si nécessaire (bfly3, bfly5)
        }
        
        m *= p;      // Taille du prochain bloc
        f_ptr -= 2;  // On remonte aux facteurs précédents
    }
}

/* Allocation corrigée pour éviter la corruption mémoire */
kiss_fft_cfg kiss_fft_alloc(int nfft, int inverse_fft, void * mem, size_t * lenmem) {
    size_t needed = sizeof(struct kiss_fft_state);
    
    if (lenmem) {
        if (mem == NULL) {
            *lenmem = needed;
            return NULL;
        }
        if (*lenmem < needed) return NULL;
    }

    kiss_fft_cfg st = (kiss_fft_cfg)KISS_FFT_MALLOC(needed);
    if (st) {
        st->nfft = nfft;
        st->inverse = inverse_fft;
        
        // COPIE SECURISÉE : On copie dans les tableaux fixes de la structure
        if (nfft <= 512) {
            memcpy(st->twiddles, g_twiddles, sizeof(kiss_fft_cpx) * nfft);
        }
        // Copie des facteurs (10 int pour 5 étages p,m)
        memcpy(st->factors, g_factors, sizeof(int) * 10);
    }
    return st;
}

void kiss_fft_stride(kiss_fft_cfg st, const kiss_fft_cpx *fin, kiss_fft_cpx *fout, int in_stride)
{
    if (fin == fout) {
        // Gestion in-place basique (buffer temporaire)
        kiss_fft_cpx * tmpbuf = (kiss_fft_cpx*)KISS_FFT_TMP_ALLOC(sizeof(kiss_fft_cpx)*st->nfft);
        kf_work_nr(tmpbuf, fin, st);
        memcpy(fout, tmpbuf, sizeof(kiss_fft_cpx)*st->nfft);
        KISS_FFT_TMP_FREE(tmpbuf);
    } else {
        // Appel direct de la version itérative
        kf_work_nr(fout, fin, st);
    }
}

void kiss_fft(kiss_fft_cfg cfg, const kiss_fft_cpx *fin, kiss_fft_cpx *fout)
{
    kiss_fft_stride(cfg, fin, fout, 1);
}

void kiss_fft_cleanup(void) { }
int kiss_fft_next_fast_size(int n) { return n; } // Simplifié pour l'exemple