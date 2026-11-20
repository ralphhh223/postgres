/* CPU performance monitoring for backend-private hash builds. */
#ifndef HALO_PMC_H
#define HALO_PMC_H

typedef struct HaloPmc HaloPmc;

extern PGDLLIMPORT char *halo_pmc_list;
extern PGDLLIMPORT bool halo_pmc_rdpmc;

extern HaloPmc *HaloPmcOpen(void);
extern bool HaloPmcStart(HaloPmc *pmc);
extern void HaloPmcSampleStart(HaloPmc *pmc);
extern void HaloPmcSampleEnd(HaloPmc *pmc);
extern void HaloPmcReport(HaloPmc *pmc);
extern void HaloPmcClose(HaloPmc *pmc);

#endif
