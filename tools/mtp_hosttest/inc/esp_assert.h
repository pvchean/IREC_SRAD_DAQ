#pragma once
#define ESP_STATIC_ASSERT(c, m) _Static_assert(c, m)
#define DRAM_DMA_ALIGNED_ATTR __attribute__((aligned(4)))
