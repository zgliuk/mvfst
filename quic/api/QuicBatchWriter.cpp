/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

#include <quic/api/QuicBatchWriter.h>

#if !FOLLY_MOBILE
#define USE_THREAD_LOCAL_BATCH_WRITER 1
#else
#define USE_THREAD_LOCAL_BATCH_WRITER 0
#endif

#if USE_THREAD_LOCAL_BATCH_WRITER
namespace {
class ThreadLocalBatchWriterCache : public folly::AsyncTimeout {
 private:
  ThreadLocalBatchWriterCache() = default;

  // we need to handle the case where the thread is being destroyed
  // while the EventBase has an outstanding timer
  struct Holder {
    Holder() = default;

    ~Holder() {
      if (ptr_) {
        ptr_->decRef();
      }
    }
    ThreadLocalBatchWriterCache* ptr_{nullptr};
  };

  void addRef() {
    ++count_;
  }

  void decRef() {
    if (--count_ == 0) {
      delete this;
    }
  }

 public:
  static ThreadLocalBatchWriterCache& getThreadLocalInstance() {
    static thread_local Holder sCache;
    if (!sCache.ptr_) {
      sCache.ptr_ = new ThreadLocalBatchWriterCache();
    }

    return *sCache.ptr_;
  }

  void timeoutExpired() noexcept override {
    timerActive_ = false;
    auto& instance = getThreadLocalInstance();
    if (instance.socket_ && instance.batchWriter_ &&
        !instance.batchWriter_->empty()) {
      // pass a default address - it is not being used by the writer
      instance.batchWriter_->write(*socket_.get(), folly::SocketAddress());
      instance.batchWriter_->reset();
    }
    decRef();
  }

  void enable(bool val) {
    if (enabled_ != val) {
      enabled_ = val;
      batchingMode_ = quic::QuicBatchingMode::BATCHING_MODE_NONE;
      batchWriter_.reset();
    }
  }

  quic::BatchWriter* FOLLY_NULLABLE getCachedWriter(
      quic::QuicBatchingMode mode,
      const std::chrono::microseconds& threadLocalDelay) {
    enabled_ = true;
    threadLocalDelay_ = threadLocalDelay;

    if (mode == batchingMode_) {
      return batchWriter_.release();
    }

    batchingMode_ = mode;
    batchWriter_.reset();

    return nullptr;
  }

  void setCachedWriter(quic::BatchWriter* writer) {
    if (enabled_) {
      auto* evb = writer->evb();

      if (evb && !socket_) {
        auto fd = writer->getAndResetFd();
        if (fd >= 0) {
          socket_ = std::make_unique<folly::AsyncUDPSocket>(evb);
          socket_->setFD(
              folly::NetworkSocket(fd),
              folly::AsyncUDPSocket::FDOwnership::OWNS);
        }
        attachTimeoutManager(evb);
      }

      batchWriter_.reset(writer);

      // start the timer if not active
      if (evb && socket_ && !timerActive_) {
        addRef();
        timerActive_ = true;
        evb->scheduleTimeoutHighRes(this, threadLocalDelay_);
      }
    } else {
      delete writer;
    }
  }

 private:
  std::atomic<uint32_t> count_{1};
  bool enabled_{false};
  bool timerActive_{false};
  std::chrono::microseconds threadLocalDelay_{1000};
  quic::QuicBatchingMode batchingMode_{
      quic::QuicBatchingMode::BATCHING_MODE_NONE};
  // this is just an  std::unique_ptr
  std::unique_ptr<quic::BatchWriter> batchWriter_;
  std::unique_ptr<folly::AsyncUDPSocket> socket_;
};
} // namespace
#endif

namespace quic {
// BatchWriter
bool BatchWriter::needsFlush(size_t /*unused*/) {
  return false;
}

// SinglePacketBatchWriter
void SinglePacketBatchWriter::reset() {
  buf_.reset();
}

bool SinglePacketBatchWriter::append(
    std::unique_ptr<folly::IOBuf>&& buf,
    size_t /*unused*/,
    const folly::SocketAddress& /*unused*/,
    folly::AsyncUDPSocket* /*unused*/) {
  buf_ = std::move(buf);

  // needs to be flushed
  return true;
}

ssize_t SinglePacketBatchWriter::write(
    folly::AsyncUDPSocket& sock,
    const folly::SocketAddress& address) {
  return sock.write(address, buf_);
}

// GSOPacketBatchWriter
GSOPacketBatchWriter::GSOPacketBatchWriter(size_t maxBufs)
    : maxBufs_(maxBufs) {}

void GSOPacketBatchWriter::reset() {
  buf_.reset(nullptr);
  currBufs_ = 0;
  prevSize_ = 0;
}

bool GSOPacketBatchWriter::needsFlush(size_t size) {
  // if we get a buffer with a size that is greater
  // than the prev one we need to flush
  return (prevSize_ && (size > prevSize_));
}

bool GSOPacketBatchWriter::append(
    std::unique_ptr<folly::IOBuf>&& buf,
    size_t size,
    const folly::SocketAddress& /*unused*/,
    folly::AsyncUDPSocket* /*unused*/) {
  // first buffer
  if (!buf_) {
    DCHECK_EQ(currBufs_, 0);
    buf_ = std::move(buf);
    prevSize_ = size;
    currBufs_ = 1;

    return false; // continue
  }

  // now we've got an additional buffer
  // append it to the chain
  buf_->prependChain(std::move(buf));
  currBufs_++;

  // see if we've added a different size
  if (size != prevSize_) {
    CHECK_LT(size, prevSize_);
    return true;
  }

  // reached max buffers
  if (FOLLY_UNLIKELY(currBufs_ == maxBufs_)) {
    return true;
  }

  // does not need to be flushed yet
  return false;
}

ssize_t GSOPacketBatchWriter::write(
    folly::AsyncUDPSocket& sock,
    const folly::SocketAddress& address) {
  return (currBufs_ > 1)
      ? sock.writeGSO(address, buf_, static_cast<int>(prevSize_))
      : sock.write(address, buf_);
}

GSOInplacePacketBatchWriter::GSOInplacePacketBatchWriter(
    QuicConnectionStateBase& conn,
    size_t maxPackets)
    : conn_(conn), maxPackets_(maxPackets) {}

void GSOInplacePacketBatchWriter::reset() {
  lastPacketEnd_ = nullptr;
  prevSize_ = 0;
  numPackets_ = 0;
}

bool GSOInplacePacketBatchWriter::needsFlush(size_t size) {
  return prevSize_ && size > prevSize_;
}

bool GSOInplacePacketBatchWriter::append(
    std::unique_ptr<folly::IOBuf>&& /*buf*/,
    size_t size,
    const folly::SocketAddress& /* addr */,
    folly::AsyncUDPSocket* /* sock */) {
  CHECK(!needsFlush(size));
  ScopedBufAccessor scopedBufAccessor(conn_.bufAccessor);
  auto& buf = scopedBufAccessor.buf();
  if (!lastPacketEnd_) {
    CHECK(prevSize_ == 0 && numPackets_ == 0);
    prevSize_ = size;
    lastPacketEnd_ = buf->tail();
    numPackets_ = 1;
    return false;
  }

  CHECK(prevSize_ && prevSize_ >= size);
  ++numPackets_;
  lastPacketEnd_ = buf->tail();
  if (prevSize_ > size || numPackets_ == maxPackets_) {
    return true;
  }
  return false;
}

/**
 * Write the buffer owned by conn_.bufAccessor to the sock, until
 * lastPacketEnd_. After write, everything in the buffer after lastPacketEnd_
 * will be moved to the beginning of the buffer, and buffer will be returned to
 * conn_.bufAccessor.
 */
ssize_t GSOInplacePacketBatchWriter::write(
    folly::AsyncUDPSocket& sock,
    const folly::SocketAddress& address) {
  ScopedBufAccessor scopedBufAccessor(conn_.bufAccessor);
  CHECK(lastPacketEnd_);
  auto& buf = scopedBufAccessor.buf();
  CHECK(!buf->isChained());
  CHECK(lastPacketEnd_ >= buf->data() && lastPacketEnd_ <= buf->tail())
      << "lastPacketEnd_=" << (long)lastPacketEnd_
      << " data=" << (long)buf->data() << " tail=" << (long)buf->tail();
  auto diffToEnd = buf->tail() - lastPacketEnd_;
  CHECK(
      diffToEnd >= 0 &&
      static_cast<uint64_t>(diffToEnd) <= conn_.udpSendPacketLen);
  auto diffToStart = lastPacketEnd_ - buf->data();
  buf->trimEnd(diffToEnd);
  auto bytesWritten = (numPackets_ > 1)
      ? sock.writeGSO(address, buf, static_cast<int>(prevSize_))
      : sock.write(address, buf);
  /**
   * If there is one more bytes after lastPacketEnd_, that means there is a
   * packet we choose not to write in this batch (e.g., it has a size larger
   * than all existing packets in this batch). So after the socket write, we
   * need to move that packet from the middle of the buffer to the beginning of
   * the buffer so make sure we maximize the buffer space. An alternative here
   * is to writem to write everything out in the previous sock write call. But
   * that needs a much bigger change in the IoBufQuicBatch API.
   */
  if (diffToEnd) {
    buf->trimStart(diffToStart);
    buf->append(diffToEnd);
    buf->retreat(diffToStart);
    CHECK(buf->length() <= conn_.udpSendPacketLen);
    CHECK(0 == buf->headroom());
  } else {
    buf->clear();
  }
  reset();
  return bytesWritten;
}

bool GSOInplacePacketBatchWriter::empty() const {
  return numPackets_ == 0;
}

size_t GSOInplacePacketBatchWriter::size() const {
  if (empty()) {
    return 0;
  }
  ScopedBufAccessor scopedBufAccessor(conn_.bufAccessor);
  CHECK(lastPacketEnd_);
  auto& buf = scopedBufAccessor.buf();
  CHECK(lastPacketEnd_ >= buf->data() && lastPacketEnd_ <= buf->tail());
  size_t ret = lastPacketEnd_ - buf->data();
  return ret;
}

// SendmmsgPacketBatchWriter
SendmmsgPacketBatchWriter::SendmmsgPacketBatchWriter(size_t maxBufs)
    : maxBufs_(maxBufs) {
  bufs_.reserve(maxBufs);
}

bool SendmmsgPacketBatchWriter::empty() const {
  return !currSize_;
}

size_t SendmmsgPacketBatchWriter::size() const {
  return currSize_;
}

void SendmmsgPacketBatchWriter::reset() {
  bufs_.clear();
  currSize_ = 0;
}

bool SendmmsgPacketBatchWriter::append(
    std::unique_ptr<folly::IOBuf>&& buf,
    size_t size,
    const folly::SocketAddress& /*unused*/,
    folly::AsyncUDPSocket* /*unused*/) {
  CHECK_LT(bufs_.size(), maxBufs_);
  bufs_.emplace_back(std::move(buf));
  currSize_ += size;

  // reached max buffers
  if (FOLLY_UNLIKELY(bufs_.size() == maxBufs_)) {
    return true;
  }

  // does not need to be flushed yet
  return false;
}

ssize_t SendmmsgPacketBatchWriter::write(
    folly::AsyncUDPSocket& sock,
    const folly::SocketAddress& address) {
  CHECK_GT(bufs_.size(), 0);
  if (bufs_.size() == 1) {
    return sock.write(address, bufs_[0]);
  }

  int ret = sock.writem(
      folly::range(&address, &address + 1), bufs_.data(), bufs_.size());

  if (ret <= 0) {
    return ret;
  }

  if (static_cast<size_t>(ret) == bufs_.size()) {
    return currSize_;
  }

  // this is a partial write - we just need to
  // return a different number than currSize_
  return 0;
}

// SendmmsgGSOPacketBatchWriter
SendmmsgGSOPacketBatchWriter::SendmmsgGSOPacketBatchWriter(size_t maxBufs)
    : maxBufs_(maxBufs) {
  bufs_.reserve(maxBufs);
}

bool SendmmsgGSOPacketBatchWriter::empty() const {
  return !currSize_;
}

size_t SendmmsgGSOPacketBatchWriter::size() const {
  return currSize_;
}

void SendmmsgGSOPacketBatchWriter::reset() {
  bufs_.clear();
  gso_.clear();
  addrs_.clear();
  currBufs_ = 0;
  currSize_ = 0;
  prevSize_ = 0;
}

bool SendmmsgGSOPacketBatchWriter::append(
    std::unique_ptr<folly::IOBuf>&& buf,
    size_t size,
    const folly::SocketAddress& addr,
    folly::AsyncUDPSocket* sock) {
  setSock(sock);
  currSize_ += size;
  // see if we need to start a new chain
  if (size > prevSize_ ||
      (!addrs_.empty() && (addr != addrs_[addrs_.size() - 1]))) {
    bufs_.emplace_back(std::move(buf));
    // set the gso_ value to 0 for now
    // this will change if we append to this chain
    gso_.emplace_back(0);
    addrs_.emplace_back(addr);
    prevSize_ = size;
    currBufs_++;

    // reached max buffers
    if (FOLLY_UNLIKELY(currBufs_ == maxBufs_)) {
      return true;
    }

    return false;
  }

  gso_.back() = prevSize_;
  bufs_.back()->prependChain(std::move(buf));
  currBufs_++;

  // reached max buffers
  if (FOLLY_UNLIKELY(currBufs_ == maxBufs_)) {
    return true;
  }

  if (size < prevSize_) {
    // reset the prevSize_ so in the next loop
    // we will start a new chain
    prevSize_ = 0;
  }

  return false;
}

ssize_t SendmmsgGSOPacketBatchWriter::write(
    folly::AsyncUDPSocket& sock,
    const folly::SocketAddress& /*unused*/) {
  CHECK_GT(bufs_.size(), 0);
  if (bufs_.size() == 1) {
    return (currBufs_ > 1) ? sock.writeGSO(addrs_[0], bufs_[0], gso_[0])
                           : sock.write(addrs_[0], bufs_[0]);
  }

  int ret = sock.writemGSO(
      folly::range(addrs_.data(), addrs_.data() + addrs_.size()),
      bufs_.data(),
      bufs_.size(),
      gso_.data());

  if (ret <= 0) {
    return ret;
  }

  if (static_cast<size_t>(ret) == bufs_.size()) {
    return currSize_;
  }

  // this is a partial write - we just need to
  // return a different number than currSize_
  return 0;
}

// BatchWriterDeleter
void BatchWriterDeleter::operator()(BatchWriter* batchWriter) {
#if USE_THREAD_LOCAL_BATCH_WRITER
  ThreadLocalBatchWriterCache::getThreadLocalInstance().setCachedWriter(
      batchWriter);
#else
  delete batchWriter;
#endif
}

// BatchWriterFactory
BatchWriterPtr BatchWriterFactory::makeBatchWriter(
    folly::AsyncUDPSocket& sock,
    const quic::QuicBatchingMode& batchingMode,
    uint32_t batchSize,
    bool useThreadLocal,
    const std::chrono::microseconds& threadLocalDelay,
    DataPathType dataPathType,
    QuicConnectionStateBase& conn) {
#if USE_THREAD_LOCAL_BATCH_WRITER
  if (useThreadLocal &&
      (batchingMode == quic::QuicBatchingMode::BATCHING_MODE_SENDMMSG_GSO) &&
      sock.getGSO() >= 0) {
    BatchWriterPtr ret(
        ThreadLocalBatchWriterCache::getThreadLocalInstance().getCachedWriter(
            batchingMode, threadLocalDelay));

    if (ret) {
      return ret;
    }
  } else {
    ThreadLocalBatchWriterCache::getThreadLocalInstance().enable(false);
  }
#else
  (void)useThreadLocal;
#endif

  switch (batchingMode) {
    case quic::QuicBatchingMode::BATCHING_MODE_NONE:
      return BatchWriterPtr(new SinglePacketBatchWriter());
    case quic::QuicBatchingMode::BATCHING_MODE_GSO: {
      if (sock.getGSO() >= 0) {
        if (dataPathType == DataPathType::ChainedMemory) {
          return BatchWriterPtr(new GSOPacketBatchWriter(batchSize));
        }
        return BatchWriterPtr(new GSOInplacePacketBatchWriter(conn, batchSize));
      }

      return BatchWriterPtr(new SinglePacketBatchWriter());
    }
    case quic::QuicBatchingMode::BATCHING_MODE_SENDMMSG:
      return BatchWriterPtr(new SendmmsgPacketBatchWriter(batchSize));
    case quic::QuicBatchingMode::BATCHING_MODE_SENDMMSG_GSO: {
      if (sock.getGSO() >= 0) {
        return BatchWriterPtr(new SendmmsgGSOPacketBatchWriter(batchSize));
      }

      return BatchWriterPtr(new SendmmsgPacketBatchWriter(batchSize));
    }
      // no default so we can catch missing case at compile time
  }

  folly::assume_unreachable();
}

} // namespace quic
