#pragma once
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace Common {
/* A lookup retains an object through concurrent handle deletion. Destruction
 * happens after releasing the table lock, so callbacks may safely look up handles. */
template<class T,class Key=int> class SharedHandleTable {
    mutable std::shared_mutex mutex;
    std::unordered_map<Key,std::shared_ptr<T>> objects;
public:
    void Insert(Key key,std::shared_ptr<T> object) {
        std::unique_lock lock{mutex};
        objects.emplace(key,std::move(object));
    }
    std::shared_ptr<T> Find(Key key) const {
        std::shared_lock lock{mutex};
        auto it=objects.find(key);
        return it==objects.end() ? nullptr : it->second;
    }
    bool Erase(Key key) {
        std::shared_ptr<T> removed;
        {
            std::unique_lock lock{mutex};
            auto it=objects.find(key);
            if (it==objects.end()) return false;
            removed=std::move(it->second);
            objects.erase(it);
        }
        return true;
    }
};
}
