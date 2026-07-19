# 一、Copy-On-Write（COW）体现在哪里
## 1. 核心代码位置：`AddBuffer` 函数
```cpp
if (buffers_map_.Get(ch_id, &existing)) {
    // Copy-on-write: atomically publish the extended vector.
    BufferVector copy = *existing; // 1. 拷贝原有整个weak_ptr数组
    copy.emplace_back(std::move(buffer)); // 2. 在副本上新增buffer
    buffers_map_.Set(ch_id, std::move(copy)); // 3. 原子替换map中的旧vector
} else {
    buffers_map_.Set(ch_id, BufferVector{std::move(buffer)});
}
```
### COW完整逻辑拆解：
1. **读不拷贝**
    `buffers_map_.Get()` 只拿到原有 `BufferVector* existing` 裸指针，**直接读原容器，无拷贝**；`Dispatch` 全程只读，不会触发复制。
2. **修改必拷贝（Copy）**
    只要需要往频道的buffer列表新增一个weak_ptr，**绝不直接修改map里现存的vector**，而是先完整复制一份 `BufferVector copy = *existing`，所有修改操作（emplace_back）只作用在**副本**上。
3. **原子发布新副本（Write）**
    修改完副本后，调用 `buffers_map_.Set(ch_id, std::move(copy))`，通过 `AtomicHashMap` 的原子CAS操作，一次性把map里指向旧vector的指针替换成新vector。
4. 旧vector自动释放
    旧的vector如果没有其他引用，替换后会被销毁；而正在执行 `Dispatch` 的线程如果已经提前拷贝了快照，还能继续安全访问旧版本数据。

### 为什么必须COW，不能直接原地push_back？
`AtomicHashMap` 底层是无锁原子读写，允许多线程并发 `Get`（读）。
如果直接 `existing->emplace_back()` 修改map内原vector：
- vector扩容、迭代器失效；
- 并发Dispatch线程正在遍历这个vector，会出现**数据竞争、崩溃、迭代器失效**；
COW把“修改”隔离到副本，原vector只读，读线程全程无风险。

# 二、`re-enters Dispatch sees a consistent view` 含义
## 对应代码：Dispatch里做快照拷贝
```cpp
BufferVector* buffers_ptr = nullptr;
if (!buffers_map_.Get(channel_id, &buffers_ptr)) return false;
BufferVector snapshot = *buffers_ptr; // 拷贝一份当前weak_ptr快照

for (auto& buffer_wptr : snapshot) { ... }
```
### 场景：Dispatch可重入（re-entrant）
重入场景举例：
1. 线程A执行 `Dispatch(ch1, msg)`，拿到buffers_ptr，拷贝snapshot，开始遍历buffer；
2. 在遍历循环内部，某个 `buffer->Fill(msg)` 触发回调/订阅逻辑，**回调内部再次调用 `Dispatch(ch1, another_msg)`**（递归重入Dispatch）；
3. 重入的Dispatch内部会调用 `AddBuffer(ch1)` 新增缓存，通过COW生成**全新的BufferVector**并替换进map。

### 如果不做snapshot拷贝（直接遍历`*buffers_ptr`）会出现的问题：
重入AddBuffer后，map里原vector被原子替换成新副本，此时外层循环还在迭代旧vector：看似能跑，但如果旧vector生命周期被释放，会野指针崩溃；

### snapshot保证一致视图（consistent view）
执行Dispatch时**立刻复制当前map里的vector副本 snapshot**，后续循环遍历只操作本地快照，和map内的原始vector完全解耦：
1. 哪怕中途重入Dispatch、调用AddBuffer新增缓存、map底层vector被替换；
2. 当前外层循环遍历的还是**调用Dispatch瞬间那一刻的频道缓存列表快照**；
3. 遍历过程中列表元素不会增减、不会失效，视图全程稳定一致；
4. 重入的Dispatch看到的是更新后的map数据，但外层循环不受干扰，互不影响。

# 三、补充区分两处拷贝（容易混淆）
1. **AddBuffer中的COW拷贝**：修改map共享容器用，解决多线程并发写map的竞争；
2. **Dispatch中的snapshot拷贝**：遍历前快照本地副本，解决Dispatch重入、并发修改导致遍历视图不稳定；
二者作用不同，相辅相成。

# 极简总结
1. Copy-On-Write 在 `AddBuffer`：修改频道buffer列表前先复制vector副本，只改副本，原子替换map内部容器，保证读线程无锁安全；
2. consistent view 快照在 `Dispatch`：进入分发逻辑立刻拷贝一份本地weak_ptr列表，即使分发过程中递归重入Dispatch并新增buffer，当前遍历循环使用的还是调用瞬间固定的缓存列表，遍历过程视图不变、安全无崩溃。