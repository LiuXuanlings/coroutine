#ifndef MINICYBER_BASE_ATOMIC_HASH_MAP_H_
#define MINICYBER_BASE_ATOMIC_HASH_MAP_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace minicyber {

// =============================================================================
// 无锁固定大小哈希表 (Lock-free Fixed-size Hash Map)
//
// 模板参数：
//   K: 键类型，必须是整型 (integral)
//   V: 值类型
//   TableSize: 桶数量，必须是 2 的幂 (便于位运算取模)
//
// 设计要点：
//   - 哈希函数：key & (TableSize - 1)，快速位运算
//   - 冲突解决：每个桶使用排序链表 (按 key 排序)
//   - 无锁机制：CAS 操作插入节点和更新值
//   - 适用场景：channel数量有限且固定的场景 (如 DataDispatcher)
// =============================================================================
template <typename K, typename V, std::size_t TableSize = 128,
          // C++11 SFINAE：
          // 1. 校验条件：K为整型 && TableSize是2的幂，二者需同时成立
          // 2. enable_if逻辑：条件true则导出int类型；false则不存在::type成员，触发SFINAE，编译器直接跳过该重载
          // 3. std::enable_if<...>::type 展开后是 int，代表一个无名模板参数, C++ 模板语法：所有模板参数都必须提供实参，要么调用时手动传入，要么给默认值；
          //    =0作用：作为模板形参时，给无名int参数赋默认值，调用无需传参
          // 4. 前缀typename必须写：::type是依赖模板的嵌套类型，告知编译器这是类型而非变量
          typename std::enable_if<std::is_integral<K>::value &&
                                      (TableSize & (TableSize - 1)) == 0,
                                  int>::type = 0>
class AtomicHashMap {
 public:
  AtomicHashMap() : capacity_(TableSize), mode_num_(capacity_ - 1) {}
  AtomicHashMap(const AtomicHashMap& other) = delete;
  AtomicHashMap& operator=(const AtomicHashMap& other) = delete;
  ~AtomicHashMap() = default;

  // 检查 key 是否存在
  bool Has(K key) {
    uint64_t index = key & mode_num_;
    return table_[index].Has(key);
  }

  // 获取值指针（成功返回 true，value 指向有效地址）
  bool Get(K key, V** value) {
    uint64_t index = key & mode_num_;
    return table_[index].Get(key, value);
  }

  // 获取值副本（成功返回 true，value 被赋值）
  bool Get(K key, V* value) {
    uint64_t index = key & mode_num_;
    V* val = nullptr;
    bool res = table_[index].Get(key, &val);
    if (res) {
      *value = *val;
    }
    return res;
  }

  // 设置默认值（key 存在则更新，不存在则插入默认值）
  void Set(K key) {
    uint64_t index = key & mode_num_;
    table_[index].Insert(key);
  }

  // 设置值（拷贝语义）
  void Set(K key, const V& value) {
    uint64_t index = key & mode_num_;
    table_[index].Insert(key, value);
  }

  // 设置值（移动语义）
  void Set(K key, V&& value) {
    uint64_t index = key & mode_num_;
    table_[index].Insert(key, std::forward<V>(value));
  }

 private:
  // =============================================================================
  // 链表节点 (Entry)
  // =============================================================================
  struct Entry {
    Entry() = default;
    explicit Entry(K k) : key(k) {
      value_ptr.store(new V(), std::memory_order_release);
    }
    Entry(K k, const V& value) : key(k) {
      value_ptr.store(new V(value), std::memory_order_release);
    }
    Entry(K k, V&& value) : key(k) {
      value_ptr.store(new V(std::forward<V>(value)), std::memory_order_release);
    }
    ~Entry() {
      delete value_ptr.load(std::memory_order_acquire);
    }

    K key = 0;
    std::atomic<V*> value_ptr{nullptr};
    std::atomic<Entry*> next{nullptr};
  };

  // =============================================================================
  // 哈希桶 (Bucket) - 使用排序链表解决冲突
  // =============================================================================
  class Bucket {
   public:
    Bucket() : head_(new Entry()) {}
    ~Bucket() {
      Entry* ite = head_;
      while (ite) {
        auto tmp = ite->next.load(std::memory_order_acquire);
        delete ite;
        ite = tmp;
      }
    }

    // 检查 key 是否存在
    bool Has(K key) {
      Entry* m_target = head_->next.load(std::memory_order_acquire);
      while (Entry* target = m_target) {
        if (target->key < key) {
          m_target = target->next.load(std::memory_order_acquire);
          continue;
        } else {
          return target->key == key;
        }
      }
      return false;
    }

    // 查找 key，返回前驱节点和目标节点
    // 返回 true 表示找到，false 表示未找到但给出插入位置
    bool Find(K key, Entry** prev_ptr, Entry** target_ptr) {
      Entry* prev = head_;
      Entry* m_target = head_->next.load(std::memory_order_acquire);
      while (Entry* target = m_target) {
        if (target->key == key) {
          *prev_ptr = prev;
          *target_ptr = target;
          return true;
        } else if (target->key > key) {
          // 链表按 key 排序，target->key > key 说明不存在
          *prev_ptr = prev;
          *target_ptr = target;
          return false;
        } else {
          prev = target;
          m_target = target->next.load(std::memory_order_acquire);
        }
      }
      *prev_ptr = prev;
      *target_ptr = nullptr;
      return false;
    }

    // 插入或更新（拷贝语义）
    void Insert(K key, const V& value) {
      Entry* prev = nullptr;
      Entry* target = nullptr;
      Entry* new_entry = nullptr;
      V* new_value = nullptr;

      while (true) {
        if (Find(key, &prev, &target)) {
          // Key 已存在，更新值
          if (!new_value) {
            new_value = new V(value);
          }
          auto old_val_ptr = target->value_ptr.load(std::memory_order_acquire);
          if (target->value_ptr.compare_exchange_strong(
                  old_val_ptr, new_value,
                  std::memory_order_acq_rel,
                  std::memory_order_relaxed)) {
            delete old_val_ptr;
            if (new_entry) {
              delete new_entry;
              new_entry = nullptr;
            }
            return;
          }
          // CAS 失败，重试
          continue;
        } else {
          // Key 不存在，插入新节点
          if (!new_entry) {
            new_entry = new Entry(key, value);
          }
          new_entry->next.store(target, std::memory_order_release);
          if (prev->next.compare_exchange_strong(
                  target, new_entry,
                  std::memory_order_acq_rel,
                  std::memory_order_relaxed)) {
            // 插入成功
            if (new_value) {
              delete new_value;
              new_value = nullptr;
            }
            return;
          }
          // 其他线程已抢先插入了一个Entry，重试
        }
      }
    }

    // 插入或更新（移动语义）
    void Insert(K key, V&& value) {
      Entry* prev = nullptr;
      Entry* target = nullptr;
      Entry* new_entry = nullptr;
      V* new_value = nullptr;

      while (true) {
        if (Find(key, &prev, &target)) {
          // Key 已存在，更新值
          if (!new_value) {
            new_value = new V(std::forward<V>(value));
          }
          auto old_val_ptr = target->value_ptr.load(std::memory_order_acquire);
          if (target->value_ptr.compare_exchange_strong(
                  old_val_ptr, new_value,
                  std::memory_order_acq_rel,
                  std::memory_order_relaxed)) {
            delete old_val_ptr;
            if (new_entry) {
              delete new_entry;
              new_entry = nullptr;
            }
            return;
          }
          continue;
        } else {
          // Key 不存在，插入新节点
          if (!new_entry) {
            new_entry = new Entry(key, std::forward<V>(value));
          }
          new_entry->next.store(target, std::memory_order_release);
          if (prev->next.compare_exchange_strong(
                  target, new_entry,
                  std::memory_order_acq_rel,
                  std::memory_order_relaxed)) {
            if (new_value) {
              delete new_value;
              new_value = nullptr;
            }
            return;
          }
        }
      }
    }

    // 插入默认值
    void Insert(K key) {
      Entry* prev = nullptr;
      Entry* target = nullptr;
      Entry* new_entry = nullptr;
      V* new_value = nullptr;

      while (true) {
        if (Find(key, &prev, &target)) {
          // Key 已存在，更新为默认值
          if (!new_value) {
            new_value = new V();
          }
          auto old_val_ptr = target->value_ptr.load(std::memory_order_acquire);
          if (target->value_ptr.compare_exchange_strong(
                  old_val_ptr, new_value,
                  std::memory_order_acq_rel,
                  std::memory_order_relaxed)) {
            delete old_val_ptr;
            if (new_entry) {
              delete new_entry;
              new_entry = nullptr;
            }
            return;
          }
          continue;
        } else {
          // Key 不存在，插入新节点
          if (!new_entry) {
            new_entry = new Entry(key);
          }
          new_entry->next.store(target, std::memory_order_release);
          if (prev->next.compare_exchange_strong(
                  target, new_entry,
                  std::memory_order_acq_rel,
                  std::memory_order_relaxed)) {
            if (new_value) {
              delete new_value;
              new_value = nullptr;
            }
            return;
          }
        }
      }
    }

    // 获取值指针
    bool Get(K key, V** value) {
      Entry* prev = nullptr;
      Entry* target = nullptr;
      if (Find(key, &prev, &target)) {
        *value = target->value_ptr.load(std::memory_order_acquire);
        return true;
      }
      return false;
    }

   private:
    Entry* head_;  // 哨兵头节点
  };

 private:
  Bucket table_[TableSize];    // 哈希桶数组
  uint64_t capacity_;          // 容量 (TableSize)
  uint64_t mode_num_;          // 取模掩码 (TableSize - 1)
};

}  // namespace minicyber

#endif  // MINICYBER_BASE_ATOMIC_HASH_MAP_H_
