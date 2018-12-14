// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fbl/algorithm.h>
#include <fbl/function.h>
#include <unittest/unittest.h>

#include "test_thread.h"
#include "userpager.h"

namespace pager_tests {

static bool check_buffer_data(Vmo* vmo, uint64_t offset,
                              uint64_t len, const void* data, bool check_vmar) {
    return check_vmar ? vmo->CheckVmar(offset, len, data) : vmo->CheckVmo(offset, len, data);
}

static bool check_buffer(Vmo* vmo, uint64_t offset, uint64_t len, bool check_vmar) {
    return check_vmar ? vmo->CheckVmar(offset, len) : vmo->CheckVmo(offset, len);
}

// Simple test that checks that a single thread can access a single page
bool single_page_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    TestThread t([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 0, 1, check_vmar);
    });

    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

    ASSERT_TRUE(t.Wait());

    END_TEST;
}

// Tests that pre-supplied pages don't result in requests
bool presupply_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

    TestThread t([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 0, 1, check_vmar);
    });

    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(t.Wait());

    ASSERT_FALSE(pager.WaitForPageRead(vmo, 0, 1, 0));

    END_TEST;
}

// Tests that supplies between the request and reading the port
// causes the request to be aborted
bool early_supply_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(2, &vmo));

    TestThread t1([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 0, 1, check_vmar);
    });
    // Use a second thread to make sure the queue of requests is flushed
    TestThread t2([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 1, 1, check_vmar);
    });

    ASSERT_TRUE(t1.Start());
    ASSERT_TRUE(t1.WaitForBlocked());
    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));
    ASSERT_TRUE(t1.Wait());

    ASSERT_TRUE(t2.Start());
    ASSERT_TRUE(t2.WaitForBlocked());
    ASSERT_TRUE(pager.WaitForPageRead(vmo, 1, 1, ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.SupplyPages(vmo, 1, 1));
    ASSERT_TRUE(t2.Wait());

    ASSERT_FALSE(pager.WaitForPageRead(vmo, 0, 1, 0));

    END_TEST;
}

// Checks that a single thread can sequentially access multiple pages
bool sequential_multipage_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    constexpr uint32_t kNumPages = 32;
    ASSERT_TRUE(pager.CreateVmo(kNumPages, &vmo));

    TestThread t([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 0, kNumPages, check_vmar);
    });

    ASSERT_TRUE(t.Start());

    for (unsigned i = 0; i < kNumPages; i++) {
        ASSERT_TRUE(pager.WaitForPageRead(vmo, i, 1, ZX_TIME_INFINITE));
        ASSERT_TRUE(pager.SupplyPages(vmo, i, 1));
    }

    ASSERT_TRUE(t.Wait());

    END_TEST;
}

// Tests that multiple threads can concurrently access different pages
bool concurrent_multipage_access_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(2, &vmo));

    TestThread t([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 0, 1, check_vmar);
    });
    TestThread t2([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 1, 1, check_vmar);
    });

    ASSERT_TRUE(t.Start());
    ASSERT_TRUE(t2.Start());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.WaitForPageRead(vmo, 1, 1, ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 2));

    ASSERT_TRUE(t.Wait());
    ASSERT_TRUE(t2.Wait());

    END_TEST;
}

// Tests that multiple threads can concurrently access a single page
bool concurrent_overlapping_access_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    constexpr uint64_t kNumThreads = 32;
    fbl::unique_ptr<TestThread> threads[kNumThreads];
    for (unsigned i = 0; i < kNumThreads; i++) {
        threads[i] = fbl::make_unique<TestThread>([vmo, check_vmar]() -> bool {
            return check_buffer(vmo, 0, 1, check_vmar);
        });

        threads[i]->Start();
        ASSERT_TRUE(threads[i]->WaitForBlocked());
    }

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

    for (unsigned i = 0; i < kNumThreads; i++) {
        ASSERT_TRUE(threads[i]->Wait());
    }

    ASSERT_FALSE(pager.WaitForPageRead(vmo, 0, 1, 0));

    END_TEST;
}

// Tests that multiple threads can concurrently access multiple pages and
// be satisfied by a single supply operation.
bool bulk_single_supply_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    constexpr uint32_t kNumPages = 8;
    ASSERT_TRUE(pager.CreateVmo(kNumPages, &vmo));

    fbl::unique_ptr<TestThread> ts[kNumPages];
    for (unsigned i = 0; i < kNumPages; i++) {
        ts[i] = fbl::make_unique<TestThread>([vmo, i, check_vmar]() -> bool {
            return check_buffer(vmo, i, 1, check_vmar);
        });
        ASSERT_TRUE(ts[i]->Start());
        ASSERT_TRUE(pager.WaitForPageRead(vmo, i, 1, ZX_TIME_INFINITE));
    }

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, kNumPages));

    for (unsigned i = 0; i < kNumPages; i++) {
        ASSERT_TRUE(ts[i]->Wait());
    }

    END_TEST;
}

// Test body for odd supply tests
bool bulk_odd_supply_test_inner(bool check_vmar, bool use_src_offset) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    // Interesting supply lengths that will exercise splice logic.
    constexpr uint64_t kSupplyLengths[] = {
        2, 3, 5, 7, 37, 5, 13, 23
    };
    uint64_t sum = 0;
    for (unsigned i = 0; i < fbl::count_of(kSupplyLengths); i++) {
        sum += kSupplyLengths[i];
    }

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(sum, &vmo));

    uint64_t page_idx = 0;
    for (unsigned supply_idx = 0; supply_idx < fbl::count_of(kSupplyLengths); supply_idx++) {
        uint64_t supply_len = kSupplyLengths[supply_idx];
        uint64_t offset = page_idx;

        fbl::unique_ptr<TestThread> ts[kSupplyLengths[supply_idx]];
        for (uint64_t j = 0; j < kSupplyLengths[supply_idx]; j++) {
            uint64_t thread_offset = offset + j;
            ts[j] = fbl::make_unique<TestThread>([vmo, thread_offset, check_vmar]() -> bool {
                return check_buffer(vmo, thread_offset, 1, check_vmar);
            });
            ASSERT_TRUE(ts[j]->Start());
            ASSERT_TRUE(pager.WaitForPageRead(vmo, thread_offset, 1, ZX_TIME_INFINITE));
        }

        uint64_t src_offset = use_src_offset ? offset : 0;
        ASSERT_TRUE(pager.SupplyPages(vmo, offset, supply_len, src_offset));

        for (unsigned i = 0; i < kSupplyLengths[supply_idx]; i++) {
            ASSERT_TRUE(ts[i]->Wait());
        }

        page_idx += kSupplyLengths[supply_idx];
    }

    END_TEST;
}

// Test which exercises supply logic by supplying data in chunks of unusual length
bool bulk_odd_length_supply_test(bool check_vmar) {
    return bulk_odd_supply_test_inner(check_vmar, false);
}

// Test which exercises supply logic by supplying data in chunks of
// unusual lengths and offsets
bool bulk_odd_offset_supply_test(bool check_vmar) {
    return bulk_odd_supply_test_inner(check_vmar, true);
}

// Tests that supply doesn't overwrite existing content
bool overlap_supply_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(2, &vmo));

    zx::vmo alt_data_vmo;
    ASSERT_EQ(zx::vmo::create(ZX_PAGE_SIZE, 0, &alt_data_vmo), ZX_OK);
    uint8_t alt_data[ZX_PAGE_SIZE];
    vmo->GenerateBufferContents(alt_data, 1, 2);
    ASSERT_EQ(alt_data_vmo.write(alt_data, 0, ZX_PAGE_SIZE), ZX_OK);

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1, std::move(alt_data_vmo)));
    ASSERT_TRUE(pager.SupplyPages(vmo, 1, 1));

    TestThread t([vmo, alt_data, check_vmar]() -> bool {
        return check_buffer_data(vmo, 0, 1, alt_data, check_vmar)
                && check_buffer(vmo, 1, 1, check_vmar);
    });

    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(t.Wait());

    ASSERT_FALSE(pager.WaitForPageRead(vmo, 0, 1, 0));

    END_TEST;
}

// Tests that a pager can handle lots of pending page requests
bool many_request_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    constexpr uint32_t kNumPages = 257; // Arbitrary large number
    ASSERT_TRUE(pager.CreateVmo(kNumPages, &vmo));

    fbl::unique_ptr<TestThread> ts[kNumPages];
    for (unsigned i = 0; i < kNumPages; i++) {
        ts[i] = fbl::make_unique<TestThread>([vmo, i, check_vmar]() -> bool {
            return check_buffer(vmo, i, 1, check_vmar);
        });
        ASSERT_TRUE(ts[i]->Start());
        ASSERT_TRUE(ts[i]->WaitForBlocked());
    }

    for (unsigned i = 0; i < kNumPages; i++) {
        ASSERT_TRUE(pager.WaitForPageRead(vmo, i, 1, ZX_TIME_INFINITE));
        ASSERT_TRUE(pager.SupplyPages(vmo, i, 1));
        ASSERT_TRUE(ts[i]->Wait());
    }

    END_TEST;
}

// Tests that a pager can support creating and destroying successive vmos
bool successive_vmo_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    constexpr uint32_t kNumVmos = 64;
    for (unsigned i = 0; i < kNumVmos; i++) {
        Vmo* vmo;
        ASSERT_TRUE(pager.CreateVmo(1, &vmo));

        TestThread t([vmo]() -> bool {
            return check_buffer(vmo, 0, 1, true);
        });

        ASSERT_TRUE(t.Start());

        ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));

        ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

        ASSERT_TRUE(t.Wait());

        pager.ReleaseVmo(vmo);
    }

    END_TEST;
}

// Tests that a pager can support multiple concurrent vmos
bool multiple_concurrent_vmo_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    constexpr uint32_t kNumVmos = 8;
    Vmo* vmos[kNumVmos];
    fbl::unique_ptr<TestThread> ts[kNumVmos];
    for (unsigned i = 0; i < kNumVmos; i++) {
        ASSERT_TRUE(pager.CreateVmo(1, vmos + i));

        ts[i] = fbl::make_unique<TestThread>([vmo = vmos[i]]() -> bool {
            return check_buffer(vmo, 0, 1, true);
        });

        ASSERT_TRUE(ts[i]->Start());

        ASSERT_TRUE(pager.WaitForPageRead(vmos[i], 0, 1, ZX_TIME_INFINITE));
    }

    for (unsigned i = 0; i < kNumVmos; i++) {
        ASSERT_TRUE(pager.SupplyPages(vmos[i], 0, 1));

        ASSERT_TRUE(ts[i]->Wait());
    }

    END_TEST;
}

// Tests that unmapping a vmo while threads are blocked on a pager read
// eventually results in pagefaults.
bool vmar_unmap_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    TestThread t([vmo]() -> bool {
        return check_buffer(vmo, 0, 1, true);
    });
    ASSERT_TRUE(t.Start());
    ASSERT_TRUE(t.WaitForBlocked());

    ASSERT_TRUE(pager.UnmapVmo(vmo));
    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

    ASSERT_TRUE(t.WaitForCrash(vmo->GetBaseAddr()));

    END_TEST;
}

// Tests that replacing a vmar mapping while threads are blocked on a
// pager read results in reads to the new mapping.
bool vmar_remap_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    constexpr uint32_t kNumPages = 8;
    ASSERT_TRUE(pager.CreateVmo(kNumPages, &vmo));

    fbl::unique_ptr<TestThread> ts[kNumPages];
    for (unsigned i = 0; i < kNumPages; i++) {
        ts[i] = fbl::make_unique<TestThread>([vmo, i]() -> bool {
            return check_buffer(vmo, i, 1, true);
        });
        ASSERT_TRUE(ts[i]->Start());
    }
    for (unsigned i = 0; i < kNumPages; i++) {
        ASSERT_TRUE(ts[i]->WaitForBlocked());
    }

    zx::vmo old_vmo;
    ASSERT_TRUE(pager.ReplaceVmo(vmo, &old_vmo));

    zx::vmo tmp;
    ASSERT_EQ(zx::vmo::create(kNumPages * ZX_PAGE_SIZE, 0, &tmp), ZX_OK);
    ASSERT_EQ(tmp.op_range(ZX_VMO_OP_COMMIT, 0, kNumPages * ZX_PAGE_SIZE, nullptr, 0), ZX_OK);
    ASSERT_EQ(zx_pager_vmo_op(pager.pager(), old_vmo.get(), ZX_PAGER_OP_SUPPLY_PAGES,
                              0, kNumPages * ZX_PAGE_SIZE, tmp.get(), 0), ZX_OK);

    for (unsigned i = 0; i < kNumPages; i++) {
        uint64_t offset, length;
        ASSERT_TRUE(pager.GetPageReadRequest(vmo, ZX_TIME_INFINITE, &offset, &length));
        ASSERT_EQ(length, 1);
        ASSERT_TRUE(pager.SupplyPages(vmo, offset, 1));
        ASSERT_TRUE(ts[offset]->Wait());
    }

    END_TEST;
}

// Tests that detaching results in a complete request
bool detach_page_complete_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    ASSERT_TRUE(pager.DetachVmo(vmo));

    ASSERT_TRUE(pager.WaitForPageComplete(vmo->GetKey(), ZX_TIME_INFINITE));

    END_TEST;
}

// Tests that closing results in a complete request
bool close_page_complete_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    uint64_t key = vmo->GetKey();
    pager.ReleaseVmo(vmo);

    ASSERT_TRUE(pager.WaitForPageComplete(key, ZX_TIME_INFINITE));

    END_TEST;
}

// Tests that interrupting a read after receiving the request doesn't result in hanging threads
bool read_interrupt_late_test(bool check_vmar, bool detach) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    TestThread t([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 0, 1, check_vmar);
    });

    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));

    if (detach) {
        ASSERT_TRUE(pager.DetachVmo(vmo));
    } else {
        pager.ClosePagerHandle();
    }

    if (check_vmar) {
        ASSERT_TRUE(t.WaitForCrash(vmo->GetBaseAddr()));
    } else {
        ASSERT_TRUE(t.WaitForFailure());
    }

    if (detach) {
        ASSERT_TRUE(pager.WaitForPageComplete(vmo->GetKey(), ZX_TIME_INFINITE));
    }

    END_TEST;
}

bool read_close_interrupt_late_test(bool check_vmar) {
    return read_interrupt_late_test(check_vmar, false);
}

bool read_detach_interrupt_late_test(bool check_vmar) {
    return read_interrupt_late_test(check_vmar, true);
}

// Tests that interrupt a read before receiving requests doesn't result in hanging threads
bool read_interrupt_early_test(bool check_vmar, bool detach) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    TestThread t([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 0, 1, check_vmar);
    });

    ASSERT_TRUE(t.Start());
    ASSERT_TRUE(t.WaitForBlocked());

    if (detach) {
        ASSERT_TRUE(pager.DetachVmo(vmo));
    } else {
        pager.ClosePagerHandle();
    }

    if (check_vmar) {
        ASSERT_TRUE(t.WaitForCrash(vmo->GetBaseAddr()));
    } else {
        ASSERT_TRUE(t.WaitForFailure());
    }

    if (detach) {
        ASSERT_TRUE(pager.WaitForPageComplete(vmo->GetKey(), ZX_TIME_INFINITE));
    }

    END_TEST;
}

bool read_close_interrupt_early_test(bool check_vmar) {
    return read_interrupt_early_test(check_vmar, false);
}

bool read_detach_interrupt_early_test(bool check_vmar) {
    return read_interrupt_early_test(check_vmar, true);
}

// Checks that a thread blocked on accessing a paged vmo can be safely killed
bool thread_kill_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(2, &vmo));

    TestThread t1([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 0, 1, check_vmar);
    });
    TestThread t2([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 1, 1, check_vmar);
    });

    ASSERT_TRUE(t1.Start());
    ASSERT_TRUE(t1.WaitForBlocked());

    ASSERT_TRUE(t2.Start());
    ASSERT_TRUE(t2.WaitForBlocked());

    ASSERT_TRUE(t1.Kill());
    ASSERT_TRUE(t1.WaitForTerm());

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 2));

    ASSERT_TRUE(t2.Wait());

    END_TEST;
}

// Checks that a thread blocked on accessing a paged vmo can be safely killed
// when there is a second thread waiting for the same address.
bool thread_kill_overlap_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    TestThread t1([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 0, 1, check_vmar);
    });
    TestThread t2([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 0, 1, check_vmar);
    });

    ASSERT_TRUE(t1.Start());
    ASSERT_TRUE(t1.WaitForBlocked());

    ASSERT_TRUE(t2.Start());
    ASSERT_TRUE(t2.WaitForBlocked());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));

    ASSERT_TRUE(t1.Kill());
    ASSERT_TRUE(t1.WaitForTerm());

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

    ASSERT_TRUE(t2.Wait());

    END_TEST;
}

// Tests that closing a pager while a thread is accessing it doesn't cause
// problems (other than a page fault in the accessing thread).
bool close_pager_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(2, &vmo));

    TestThread t([vmo]() -> bool {
        return check_buffer(vmo, 0, 1, true);
    });
    ASSERT_TRUE(pager.SupplyPages(vmo, 1, 1));

    ASSERT_TRUE(t.Start());
    ASSERT_TRUE(t.WaitForBlocked());

    pager.ClosePagerHandle();

    ASSERT_TRUE(t.WaitForCrash(vmo->GetBaseAddr()));
    ASSERT_TRUE(check_buffer(vmo, 1, 1, true));

    END_TEST;
}

// Tests that closing a pager while a vmo is being detached doesn't cause problems.
bool detach_close_pager_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    ASSERT_TRUE(pager.DetachVmo(vmo));

    pager.ClosePagerHandle();

    END_TEST;
}

// Tests that closing an in use port doesn't cause issues (beyond no
// longer being able to receive requests).
bool close_port_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(2, &vmo));

    TestThread t([vmo]() -> bool {
        return check_buffer(vmo, 0, 1, true);
    });

    ASSERT_TRUE(t.Start());
    ASSERT_TRUE(t.WaitForBlocked());

    pager.ClosePortHandle();

    ASSERT_TRUE(pager.SupplyPages(vmo, 1, 1));
    ASSERT_TRUE(check_buffer(vmo, 1, 1, true));

    ASSERT_TRUE(pager.DetachVmo(vmo));
    ASSERT_TRUE(t.WaitForCrash(vmo->GetBaseAddr()));

    END_TEST;
}

// Tests reading from a clone populates the vmo
bool clone_read_from_clone_test(bool check_vmar) {
    BEGIN_TEST;
    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    auto clone = vmo->Clone();
    ASSERT_NOT_NULL(clone);

    TestThread t([clone = clone.get(), check_vmar]() -> bool {
        return check_buffer(clone, 0, 1, check_vmar);
    });

    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

    ASSERT_TRUE(t.Wait());

    END_TEST;
}

// Tests reading from the parent populates the clone
bool clone_read_from_parent_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    auto clone = vmo->Clone();
    ASSERT_NOT_NULL(clone);

    TestThread t([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 0, 1, check_vmar);
    });

    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

    ASSERT_TRUE(t.Wait());

    TestThread t2([clone = clone.get(), check_vmar]() -> bool {
        return check_buffer(clone, 0, 1, check_vmar);
    });

    ASSERT_TRUE(t2.Start());
    ASSERT_TRUE(t2.Wait());

    ASSERT_FALSE(pager.WaitForPageRead(vmo, 0, 1, 0));

    END_TEST;
}

// Tests that overlapping reads on clone and parent work
bool clone_simultanious_read_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    auto clone = vmo->Clone();
    ASSERT_NOT_NULL(clone);

    TestThread t([vmo, check_vmar]() -> bool {
        return check_buffer(vmo, 0, 1, check_vmar);
    });
    TestThread t2([clone = clone.get(), check_vmar]() -> bool {
        return check_buffer(clone, 0, 1, check_vmar);
    });

    ASSERT_TRUE(t.Start());
    ASSERT_TRUE(t2.Start());

    ASSERT_TRUE(t.WaitForBlocked());
    ASSERT_TRUE(t2.WaitForBlocked());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

    ASSERT_TRUE(t.Wait());
    ASSERT_TRUE(t2.Wait());

    ASSERT_FALSE(pager.WaitForPageRead(vmo, 0, 1, 0));

    END_TEST;
}

// Tests that overlapping reads from two clones work
bool clone_simultanious_child_read_test(bool check_vmar) {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));

    auto clone = vmo->Clone();
    ASSERT_NOT_NULL(clone);
    auto clone2 = vmo->Clone();
    ASSERT_NOT_NULL(clone2);

    TestThread t([clone = clone2.get(), check_vmar]() -> bool {
        return check_buffer(clone, 0, 1, check_vmar);
    });
    TestThread t2([clone = clone.get(), check_vmar]() -> bool {
        return check_buffer(clone, 0, 1, check_vmar);
    });

    ASSERT_TRUE(t.Start());
    ASSERT_TRUE(t2.Start());

    ASSERT_TRUE(t.WaitForBlocked());
    ASSERT_TRUE(t2.WaitForBlocked());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

    ASSERT_TRUE(t.Wait());
    ASSERT_TRUE(t2.Wait());

    ASSERT_FALSE(pager.WaitForPageRead(vmo, 0, 1, 0));

    END_TEST;
}

// Tests that commit on the clone populates things properly
bool clone_commit_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    constexpr uint64_t kNumPages = 32;
    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(kNumPages, &vmo));

    auto clone = vmo->Clone();
    ASSERT_NOT_NULL(clone);

    TestThread t([clone = clone.get()]() -> bool {
            return clone->Commit(0, kNumPages);
    });

    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, kNumPages, ZX_TIME_INFINITE));

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, kNumPages));

    ASSERT_TRUE(t.Wait());

    END_TEST;
}

// Tests that commit on the clone populates things properly if things have already been touched
bool clone_split_commit_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    constexpr uint64_t kNumPages = 4;
    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(kNumPages, &vmo));

    auto clone = vmo->Clone();
    ASSERT_NOT_NULL(clone);

    TestThread t([clone = clone.get()]() -> bool {
            return clone->Commit(0, kNumPages);
    });

    // Populate pages 1 and 2 of the parent vmo, and page 1 of the clone
    ASSERT_TRUE(pager.SupplyPages(vmo, 1, 2));
    ASSERT_TRUE(clone->CheckVmar(1, 1));

    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

    ASSERT_TRUE(pager.WaitForPageRead(vmo, kNumPages - 1, 1, ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.SupplyPages(vmo, kNumPages - 1, 1));

    ASSERT_TRUE(t.Wait());

    END_TEST;
}

// Tests that decommit on clone doesn't decommit the parent
bool clone_decommit_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(1, &vmo));
    auto clone = vmo->Clone();
    ASSERT_NOT_NULL(clone);

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));
    ASSERT_TRUE(check_buffer(clone.get(), 0, 1, true));

    ASSERT_TRUE(clone->Decommit(0, 1));

    ASSERT_TRUE(check_buffer(clone.get(), 0, 1, true));

    END_TEST;
}

// Tests that detaching the parent doesn't crash the clone
bool clone_detach_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(2, &vmo));
    auto clone = vmo->Clone();

    ASSERT_TRUE(pager.SupplyPages(vmo, 1, 1));

    TestThread t([clone = clone.get()]() -> bool {
            uint8_t data[ZX_PAGE_SIZE] = {};
            return check_buffer_data(clone, 0, 1, data, true) && check_buffer(clone, 1, 1, true);
    });
    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));

    ASSERT_TRUE(pager.DetachVmo(vmo));

    ASSERT_TRUE(pager.WaitForPageComplete(vmo->GetKey(), ZX_TIME_INFINITE));

    ASSERT_TRUE(t.Wait());

    END_TEST;
}

// Tests that a commit properly populates the whole range.
bool simple_commit_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    constexpr uint64_t kNumPages = 555;
    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(kNumPages, &vmo));

    TestThread t([vmo]() -> bool {
            return vmo->Commit(0, kNumPages);
    });

    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, kNumPages, ZX_TIME_INFINITE));

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, kNumPages));

    ASSERT_TRUE(t.Wait());

    END_TEST;
}

// Tests that a commit over a partially populated range is properly split.
bool split_commit_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    constexpr uint64_t kNumPages = 33;
    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(kNumPages, &vmo));

    ASSERT_TRUE(pager.SupplyPages(vmo, (kNumPages / 2), 1));

    TestThread t([vmo]() -> bool {
            return vmo->Commit(0, kNumPages);
    });

    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, (kNumPages / 2), ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.SupplyPages(vmo, 0, (kNumPages / 2)));

    ASSERT_TRUE(pager.WaitForPageRead(vmo, (kNumPages / 2) + 1,
                                      kNumPages / 2, ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.SupplyPages(vmo, ((kNumPages / 2) + 1), (kNumPages / 2)));

    ASSERT_TRUE(t.Wait());

    END_TEST;
}


// Tests that overlapping commits don't result in redundant requests.
bool overlap_commit_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    constexpr uint64_t kNumPages = 32;
    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(kNumPages, &vmo));

    TestThread t1([vmo]() -> bool {
            return vmo->Commit((kNumPages / 4), (kNumPages / 2));
    });
    TestThread t2([vmo]() -> bool {
            return vmo->Commit(0, kNumPages);
    });

    ASSERT_TRUE(t1.Start());
    ASSERT_TRUE(pager.WaitForPageRead(vmo, (kNumPages / 4), (kNumPages / 2), ZX_TIME_INFINITE));

    ASSERT_TRUE(t2.Start());
    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, (kNumPages / 4), ZX_TIME_INFINITE));

    ASSERT_TRUE(pager.SupplyPages(vmo, 0, (3 * kNumPages / 4)));

    ASSERT_TRUE(pager.WaitForPageRead(vmo, (3 * kNumPages / 4), (kNumPages / 4), ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.SupplyPages(vmo, (3 * kNumPages / 4), (kNumPages / 4)));

    ASSERT_TRUE(t1.Wait());
    ASSERT_TRUE(t2.Wait());

    END_TEST;
}

// Tests that overlapping commits are properly supplied
bool overlap_commit_supply_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    constexpr uint64_t kSupplyLen = 3;
    constexpr uint64_t kCommitLenA = 7;
    constexpr uint64_t kCommitLenB = 5;
    constexpr uint64_t kNumPages = kCommitLenA * kCommitLenB * kSupplyLen;
    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(kNumPages, &vmo));

    fbl::unique_ptr<TestThread> tsA[kNumPages / kCommitLenA];
    for (unsigned i = 0; i < fbl::count_of(tsA); i++) {
        tsA[i] = fbl::make_unique<TestThread>([vmo, i]() -> bool {
            return vmo->Commit(i * kCommitLenA, kCommitLenA);
        });

        ASSERT_TRUE(tsA[i]->Start());
        ASSERT_TRUE(pager.WaitForPageRead(vmo, i * kCommitLenA, kCommitLenA, ZX_TIME_INFINITE));
    }

    fbl::unique_ptr<TestThread> tsB[kNumPages / kCommitLenB];
    for (unsigned i = 0; i < fbl::count_of(tsB); i++) {
        tsB[i] = fbl::make_unique<TestThread>([vmo, i]() -> bool {
            return vmo->Commit(i * kCommitLenB, kCommitLenB);
        });

        ASSERT_TRUE(tsB[i]->Start());
        ASSERT_TRUE(tsB[i]->WaitForBlocked());
    }

    for (unsigned i = 0; i < kNumPages / kSupplyLen; i++) {
        ASSERT_TRUE(pager.SupplyPages(vmo, i * kSupplyLen, kSupplyLen));
    }

    for (unsigned i = 0; i < fbl::count_of(tsA); i++) {
        ASSERT_TRUE(tsA[i]->Wait());
    }
    for (unsigned i = 0; i < fbl::count_of(tsB); i++) {
        ASSERT_TRUE(tsB[i]->Wait());
    }

    END_TEST;
}

// Tests that a single commit can be fulfilled by multiple supplies.
bool multisupply_commit_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    constexpr uint64_t kNumPages = 32;
    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(kNumPages, &vmo));

    TestThread t([vmo]() -> bool {
            return vmo->Commit(0, kNumPages);
    });

    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, kNumPages, ZX_TIME_INFINITE));

    for (unsigned i = 0; i < kNumPages; i++) {
        ASSERT_TRUE(pager.SupplyPages(vmo, i, 1));
    }

    ASSERT_TRUE(t.Wait());

    END_TEST;
}

// Tests that a single supply can fulfil multiple commits
bool multicommit_supply_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    constexpr uint64_t kNumCommits = 5;
    constexpr uint64_t kNumSupplies = 7;
    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(kNumCommits * kNumSupplies, &vmo));

    fbl::unique_ptr<TestThread> ts[kNumCommits];
    for (unsigned i = 0; i < kNumCommits; i++) {
        ts[i] = fbl::make_unique<TestThread>([vmo, i]() -> bool {
                return vmo->Commit(i * kNumSupplies, kNumSupplies);
        });
        ASSERT_TRUE(ts[i]->Start());
        ASSERT_TRUE(pager.WaitForPageRead(vmo, i * kNumSupplies, kNumSupplies, ZX_TIME_INFINITE));
    }

    for (unsigned i = 0; i < kNumSupplies; i++) {
        ASSERT_TRUE(pager.SupplyPages(vmo, kNumCommits * i, kNumCommits));
    }

    for (unsigned i = 0; i < kNumCommits; i++) {
        ASSERT_TRUE(ts[i]->Wait());
    }

    END_TEST;
}

// Tests that redundant supplies for a single commit don't cause errors
bool commit_redundant_supply_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    constexpr uint64_t kNumPages = 8;
    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(kNumPages, &vmo));

    TestThread t([vmo]() -> bool {
            return vmo->Commit(0, kNumPages);
    });

    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, kNumPages, ZX_TIME_INFINITE));

    for (unsigned i = 1; i <= kNumPages; i++) {
        ASSERT_TRUE(pager.SupplyPages(vmo, 0, i));
    }

    ASSERT_TRUE(t.Wait());

    END_TEST;
}

// Tests that decommitting during a supply doesn't break things
bool supply_decommit_test() {
    BEGIN_TEST;

    UserPager pager;

    ASSERT_TRUE(pager.Init());

    constexpr uint64_t kNumPages = 4;
    Vmo* vmo;
    ASSERT_TRUE(pager.CreateVmo(kNumPages, &vmo));

    TestThread t([vmo]() -> bool {
            return vmo->Commit(0, kNumPages);
    });

    ASSERT_TRUE(t.Start());

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, kNumPages, ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));
    ASSERT_TRUE(vmo->Decommit(0, 1));
    ASSERT_TRUE(pager.SupplyPages(vmo, 1, kNumPages - 1));

    ASSERT_TRUE(pager.WaitForPageRead(vmo, 0, 1, ZX_TIME_INFINITE));
    ASSERT_TRUE(pager.SupplyPages(vmo, 0, 1));

    ASSERT_TRUE(t.Wait());

    END_TEST;
}


#define DEFINE_VMO_VMAR_TEST(fn_name) \
bool fn_name ##_vmar() { return fn_name(true); } \
bool fn_name ##_vmo() { return fn_name(false); } \

#define RUN_VMO_VMAR_TEST(fn_name) \
RUN_TEST(fn_name ##_vmar); \
RUN_TEST(fn_name ##_vmo);

// Tests focused on reading a paged vmo

DEFINE_VMO_VMAR_TEST(single_page_test);
DEFINE_VMO_VMAR_TEST(presupply_test);
DEFINE_VMO_VMAR_TEST(early_supply_test);
DEFINE_VMO_VMAR_TEST(sequential_multipage_test);
DEFINE_VMO_VMAR_TEST(concurrent_multipage_access_test);
DEFINE_VMO_VMAR_TEST(concurrent_overlapping_access_test);
DEFINE_VMO_VMAR_TEST(bulk_single_supply_test);
DEFINE_VMO_VMAR_TEST(bulk_odd_length_supply_test);
DEFINE_VMO_VMAR_TEST(bulk_odd_offset_supply_test);
DEFINE_VMO_VMAR_TEST(overlap_supply_test);
DEFINE_VMO_VMAR_TEST(many_request_test);

BEGIN_TEST_CASE(pager_read_tests)
RUN_VMO_VMAR_TEST(single_page_test);
RUN_VMO_VMAR_TEST(presupply_test);
RUN_VMO_VMAR_TEST(early_supply_test);
RUN_VMO_VMAR_TEST(sequential_multipage_test);
RUN_VMO_VMAR_TEST(concurrent_multipage_access_test);
RUN_VMO_VMAR_TEST(concurrent_overlapping_access_test);
RUN_VMO_VMAR_TEST(bulk_single_supply_test);
RUN_VMO_VMAR_TEST(bulk_odd_length_supply_test);
RUN_VMO_VMAR_TEST(bulk_odd_offset_supply_test);
RUN_VMO_VMAR_TEST(overlap_supply_test);
RUN_VMO_VMAR_TEST(many_request_test);
RUN_TEST(successive_vmo_test);
RUN_TEST(multiple_concurrent_vmo_test);
RUN_TEST(vmar_unmap_test);
RUN_TEST(vmar_remap_test);
END_TEST_CASE(pager_read_tests)

// Tests focused on lifecycle of pager and paged vmos

DEFINE_VMO_VMAR_TEST(read_detach_interrupt_late_test);
DEFINE_VMO_VMAR_TEST(read_close_interrupt_late_test);
DEFINE_VMO_VMAR_TEST(read_detach_interrupt_early_test);
DEFINE_VMO_VMAR_TEST(read_close_interrupt_early_test);
DEFINE_VMO_VMAR_TEST(thread_kill_test);
DEFINE_VMO_VMAR_TEST(thread_kill_overlap_test);

BEGIN_TEST_CASE(lifecycle_tests)
RUN_TEST(detach_page_complete_test);
RUN_TEST(close_page_complete_test);
RUN_VMO_VMAR_TEST(read_detach_interrupt_late_test);
RUN_VMO_VMAR_TEST(read_close_interrupt_late_test);
RUN_VMO_VMAR_TEST(read_detach_interrupt_early_test);
RUN_VMO_VMAR_TEST(read_close_interrupt_early_test);
RUN_VMO_VMAR_TEST(thread_kill_test);
RUN_VMO_VMAR_TEST(thread_kill_overlap_test);
RUN_TEST(close_pager_test);
RUN_TEST(detach_close_pager_test);
RUN_TEST(close_port_test);
END_TEST_CASE(lifecycle_tests)

// Tests focused on clones

DEFINE_VMO_VMAR_TEST(clone_read_from_clone_test);
DEFINE_VMO_VMAR_TEST(clone_read_from_parent_test);
DEFINE_VMO_VMAR_TEST(clone_simultanious_read_test);
DEFINE_VMO_VMAR_TEST(clone_simultanious_child_read_test);

BEGIN_TEST_CASE(clone_tests);
RUN_VMO_VMAR_TEST(clone_read_from_clone_test);
RUN_VMO_VMAR_TEST(clone_read_from_parent_test);
RUN_VMO_VMAR_TEST(clone_simultanious_read_test);
RUN_VMO_VMAR_TEST(clone_simultanious_child_read_test);
RUN_TEST(clone_commit_test);
RUN_TEST(clone_split_commit_test);
RUN_TEST(clone_decommit_test);
RUN_TEST(clone_detach_test);
END_TEST_CASE(clone_tests);

// Tests focused on commit/decommit

BEGIN_TEST_CASE(commit_tests)
RUN_TEST(simple_commit_test);
RUN_TEST(split_commit_test);
RUN_TEST(overlap_commit_test);
RUN_TEST(overlap_commit_supply_test);
RUN_TEST(multisupply_commit_test);
RUN_TEST(multicommit_supply_test);
RUN_TEST(commit_redundant_supply_test);
RUN_TEST(supply_decommit_test);
END_TEST_CASE(commit_tests)

} // namespace pager_tests

//TODO: Test cases which violate various syscall invalid args

#ifndef BUILD_COMBINED_TESTS
int main(int argc, char** argv) {
    if (!unittest_run_all_tests(argc, argv)) {
        return -1;
    }
    return 0;
}
#endif