#include "../gpu/shadps4/common/shared_handle_table.h"
#include <cassert>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
struct Value { int number; };
int main() {
    Common::SharedHandleTable<Value> table;
    table.Insert(1,std::make_shared<Value>(123));
    auto retained=table.Find(1);
    assert(table.Erase(1) && !table.Find(1) && retained->number==123);
    assert(!table.Erase(1));
    std::atomic<int> done{0};
    std::vector<std::thread> threads;
    for (int worker=0;worker<4;++worker) {
        threads.emplace_back([&,worker] {
            for (int i=0;i<20000;++i) {
                int key=worker*20000+i+2;
                table.Insert(key,std::make_shared<Value>(key));
                auto value=table.Find(key);
                assert(value && value->number==key && table.Erase(key));
                assert(value->number==key);
            }
            ++done;
        });
    }
    threads.emplace_back([&] {
        while (done<4) {
            for (int key=2;key<80002;key+=97) {
                auto value=table.Find(key);
                if (value) assert(value->number==key);
            }
        }
    });
    for (auto& thread:threads) thread.join();
    puts("Shared handle lifetime and 80000 concurrent insert/lookup/delete tests passed");
}
