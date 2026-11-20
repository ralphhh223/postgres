/* Linux perf events for backend-private hash builds. */
#include "postgres.h"

#include "catalog/pg_namespace_d.h"
#include "executor/spi.h"
#include "port/atomics.h"
#include "utils/halo_pmc.h"
#include "utils/lsyscache.h"
#include "utils/varlena.h"

char *halo_pmc_list = NULL;
bool halo_pmc_rdpmc = false;

#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define HALO_PMC_MAX 8
#define HALO_PMC_RETRIES 8

typedef struct HaloPmcEvent
{
	char *name;
	uint32 type;
	uint64 config;
	uint64 config1;
	uint64 config2;
	int fd;
	struct perf_event_mmap_page *page;
	bool fast;
	bool started;
	bool sample_valid;
	uint64 sample_begin;
	uint64 total;
	uint64 samples;
	uint64 skipped;
} HaloPmcEvent;

struct HaloPmc
{
	int count;
	Size page_size;
	HaloPmcEvent events[HALO_PMC_MAX];
};

typedef struct HaloPmcRead
{
	uint64 value;
	uint64 enabled;
	uint64 running;
} HaloPmcRead;

static int
open_event(HaloPmcEvent *event, bool pinned)
{
	struct perf_event_attr attr = {0};

	attr.size = sizeof(attr);
	attr.type = event->type;
	attr.config = event->config;
	attr.config1 = event->config1;
	attr.config2 = event->config2;
	attr.disabled = 1;
	attr.pinned = pinned;
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;
	attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
		PERF_FORMAT_TOTAL_TIME_RUNNING;

	return syscall(SYS_perf_event_open, &attr, 0, -1, -1,
				   PERF_FLAG_FD_CLOEXEC);
}

/* The perf mmap sequence protects index, offset and the counter width. */
static bool
read_rdpmc(const struct perf_event_mmap_page *page, uint64 *value)
{
#ifdef __x86_64__
	for (int attempt = 0; attempt < HALO_PMC_RETRIES; attempt++)
	{
		uint32 seq = __atomic_load_n(&page->lock, __ATOMIC_ACQUIRE);
		uint32 index;
		uint16 width;
		uint64 offset;
		uint32 lo, hi;
		uint64 raw;
		int shift;

		if (seq & 1)
			continue;
		index = page->index;
		width = page->pmc_width;
		offset = (uint64) page->offset;
		if (!page->cap_user_rdpmc || index == 0 || width == 0 || width > 64)
			return false;

		/* Serialize both sides so the target instructions stay between reads. */
		__asm__ volatile("lfence; rdpmc; lfence"
						 : "=a"(lo), "=d"(hi) : "c"(index - 1) : "memory");
		pg_read_barrier();
		if (seq != __atomic_load_n(&page->lock, __ATOMIC_RELAXED))
			continue;

		raw = ((uint64) hi << 32) | lo;
		shift = 64 - width;
		*value = offset + (uint64) ((int64) (raw << shift) >> shift);
		return true;
	}
#endif
	return false;
}

static void
close_event(HaloPmc *pmc, HaloPmcEvent *event)
{
	if (event->page != NULL)
	{
		munmap(event->page, pmc->page_size);
		event->page = NULL;
	}
	if (event->fd >= 0)
	{
		close(event->fd);
		event->fd = -1;
	}
}

/*
 * Resolve the database-local event catalog before opening descriptors.
 * No SPI work, allocation or catalog access is done in the sampling loop.
 */
HaloPmc *
HaloPmcOpen(void)
{
	char *list;
	List *names = NIL;
	ListCell *lc;
	HaloPmc *pmc;
	long page_size;

	if (halo_pmc_list == NULL || halo_pmc_list[0] == '\0')
		return NULL;

	list = pstrdup(halo_pmc_list);
	if (!SplitIdentifierString(list, ',', &names) ||
		list_length(names) > HALO_PMC_MAX)
	{
		ereport(INFO,
				(errmsg("halo_pmc: expected up to %d comma-separated event names",
						HALO_PMC_MAX)));
		list_free(names);
		pfree(list);
		return NULL;
	}
	if (names == NIL)
	{
		pfree(list);
		return NULL;
	}

	if (!OidIsValid(get_relname_relid("pmc_name", PG_CATALOG_NAMESPACE)))
	{
		ereport(INFO,
				(errmsg("halo_pmc: event catalog is missing"),
				 errhint("Run contrib/halo_pmc/init.sql in this database.")));
		list_free(names);
		pfree(list);
		return NULL;
	}

	pmc = palloc0(sizeof(*pmc));
	page_size = sysconf(_SC_PAGESIZE);
	pmc->page_size = page_size > 0 ? (Size) page_size : 0;

	SPI_connect();
	if (SPI_execute("SELECT name, event_type, config, config1, config2 "
					"FROM pg_catalog.pmc_name", true, 0) != SPI_OK_SELECT)
		elog(ERROR, "halo_pmc: could not read event catalog");

	foreach(lc, names)
	{
		const char *name = lfirst(lc);
		int i;

		for (i = 0; i < pmc->count; i++)
			if (pg_strcasecmp(pmc->events[i].name, name) == 0)
				break;
		if (i < pmc->count)
			continue;

		for (uint64 row = 0; row < SPI_processed; row++)
		{
			HeapTuple tuple = SPI_tuptable->vals[row];
			TupleDesc desc = SPI_tuptable->tupdesc;
			const char *entry = SPI_getvalue(tuple, desc, 1);

			if (entry != NULL && pg_strcasecmp(entry, name) == 0)
			{
				HaloPmcEvent *event = &pmc->events[pmc->count++];
				bool isnull;

				event->name = SPI_palloc(strlen(entry) + 1);
				strcpy(event->name, entry);
				event->type = DatumGetInt32(SPI_getbinval(tuple, desc, 2, &isnull));
				event->config = DatumGetInt64(SPI_getbinval(tuple, desc, 3, &isnull));
				event->config1 = DatumGetInt64(SPI_getbinval(tuple, desc, 4, &isnull));
				event->config2 = DatumGetInt64(SPI_getbinval(tuple, desc, 5, &isnull));
				event->fd = -1;
				break;
			}
		}
		if (i == pmc->count)
			ereport(INFO, (errmsg("halo_pmc: unknown event \"%s\"", name)));
	}
	SPI_finish();
	list_free(names);
	pfree(list);

	if (pmc->count == 0)
	{
		pfree(pmc);
		return NULL;
	}
	return pmc;
}

/* Called inside the caller's PG_FINALLY-protected region. */
bool
HaloPmcStart(HaloPmc *pmc)
{
	bool any_fast = false;

	for (int i = 0; i < pmc->count; i++)
	{
		HaloPmcEvent *event = &pmc->events[i];

#ifdef __x86_64__
		if (halo_pmc_rdpmc && pmc->page_size > 0)
		{
			uint64 probe;

			event->fd = open_event(event, true);
			if (event->fd >= 0)
			{
				void *page = mmap(NULL, pmc->page_size, PROT_READ, MAP_SHARED,
								  event->fd, 0);

				if (page != MAP_FAILED)
				{
					event->page = page;
					if (ioctl(event->fd, PERF_EVENT_IOC_ENABLE, 0) == 0 &&
						read_rdpmc(event->page, &probe))
					{
						event->fast = true;
						any_fast = true;
					}
				}
				if (!event->fast)
					close_event(pmc, event);
			}
		}
#endif
		if (!event->fast)
		{
			event->fd = open_event(event, false);
			if (event->fd < 0)
				ereport(INFO,
						(errmsg("halo_pmc: event %s unavailable: %m", event->name)));
		}
	}

	for (int i = 0; i < pmc->count; i++)
	{
		HaloPmcEvent *event = &pmc->events[i];

		if (event->fd < 0)
			continue;
		event->started = event->fast ||
			ioctl(event->fd, PERF_EVENT_IOC_ENABLE, 0) == 0;
		if (!event->started)
			ereport(INFO,
					(errmsg("halo_pmc: could not enable event %s: %m", event->name)));
	}
	return any_fast;
}

void
HaloPmcSampleStart(HaloPmc *pmc)
{
	for (int i = 0; i < pmc->count; i++)
	{
		HaloPmcEvent *event = &pmc->events[i];

		if (event->fast)
			event->sample_valid = read_rdpmc(event->page, &event->sample_begin);
	}
}

void
HaloPmcSampleEnd(HaloPmc *pmc)
{
	for (int i = 0; i < pmc->count; i++)
	{
		HaloPmcEvent *event = &pmc->events[i];
		uint64 end;

		if (!event->fast)
			continue;
		if (event->sample_valid && read_rdpmc(event->page, &end) &&
			end >= event->sample_begin)
		{
			event->total += end - event->sample_begin;
			event->samples++;
		}
		else
			event->skipped++;
	}
}

void
HaloPmcReport(HaloPmc *pmc)
{
	for (int i = 0; i < pmc->count; i++)
	{
		HaloPmcEvent *event = &pmc->events[i];

		if (event->started && ioctl(event->fd, PERF_EVENT_IOC_DISABLE, 0) < 0)
		{
			event->started = false;
			ereport(INFO,
					(errmsg("halo_pmc: could not disable event %s: %m", event->name)));
		}
	}
	for (int i = 0; i < pmc->count; i++)
	{
		HaloPmcEvent *event = &pmc->events[i];
		HaloPmcRead data;
		ssize_t bytes;

		if (!event->started)
			continue;
		if (event->fast)
		{
			ereport(INFO,
					(errmsg("halo_pmc: mode=rdpmc scope=hash_insert event=%s raw=" UINT64_FORMAT
							" samples=" UINT64_FORMAT " skipped=" UINT64_FORMAT,
							event->name, event->total, event->samples, event->skipped)));
			continue;
		}

		do {
			bytes = read(event->fd, &data, sizeof(data));
		} while (bytes < 0 && errno == EINTR);
		if (bytes != (ssize_t) sizeof(data) || data.running == 0)
			ereport(INFO,
					(errmsg("halo_pmc: event %s was not readable or did not run", event->name)));
		else
			ereport(INFO,
					(errmsg("halo_pmc: mode=%s scope=hash_build_loop event=%s raw=" UINT64_FORMAT
							" scaled=%.0f enabled_ns=" UINT64_FORMAT " running_ns=" UINT64_FORMAT,
							halo_pmc_rdpmc ? "read_fallback" : "read", event->name,
							data.value, (double) data.value * data.enabled / data.running,
							data.enabled, data.running)));
	}
}

void
HaloPmcClose(HaloPmc *pmc)
{
	for (int i = 0; i < pmc->count; i++)
	{
		close_event(pmc, &pmc->events[i]);
		pfree(pmc->events[i].name);
	}
	pfree(pmc);
}

#else

HaloPmc *
HaloPmcOpen(void)
{
	if (halo_pmc_list != NULL && halo_pmc_list[0] != '\0')
		ereport(INFO, (errmsg("halo_pmc: perf events are available on Linux only")));
	return NULL;
}

bool HaloPmcStart(HaloPmc *pmc) { return false; }
void HaloPmcSampleStart(HaloPmc *pmc) {}
void HaloPmcSampleEnd(HaloPmc *pmc) {}
void HaloPmcReport(HaloPmc *pmc) {}
void HaloPmcClose(HaloPmc *pmc) {}

#endif
