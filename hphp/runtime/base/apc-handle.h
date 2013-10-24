/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#ifndef incl_HPHP_APC_HANDLE_H_
#define incl_HPHP_APC_HANDLE_H_

#include "hphp/runtime/base/types.h"
#include "hphp/util/lock.h"
#include "hphp/util/hash.h"
#include "hphp/util/atomic.h"
#include "hphp/runtime/base/complex-types.h"

#if (defined(__APPLE__) || defined(__APPLE_CC__)) && (defined(__BIG_ENDIAN__) || defined(__LITTLE_ENDIAN__))
# if defined(__LITTLE_ENDIAN__)
#  undef WORDS_BIGENDIAN
# else
#  if defined(__BIG_ENDIAN__)
#   define WORDS_BIGENDIAN
#  endif
# endif
#endif

namespace HPHP {

class APCHandleStats;

///////////////////////////////////////////////////////////////////////////////

/*
 * An APCHandle is the externally visible handle for APC entities. APC entities
 * are stored in-memory and accessible to multiple requests/threads.
 * The main role of the APCHandle is to hold the type of the shared data
 * and to manage the lifetime.
 * When an APC entity is stored/added, APCHandle::Create is called.
 * The API is then very simple:
 * - toLocal() fetches an instance of the PHP object stored in the APC entity.
 * That instance will be available to the request/thread that performed the
 * call and that thread only
 * - inc/decRef are the obvious memory management API
 * - few flags accessors are provided too
 *
 * Internally the family of APC entities (all APCXXXX classes, e.g. APCString,
 * APCArray, ...) embed the handle and provide an API to return the instance
 * from the handle pointer. Examples:
 *
 *    APCString                         APCTypedValue
 *    --------------                   --------------
 *   | APCHandle    |                  | SharedData   |
 *   | StringData   |                  | APCHandle    |
 *    --------------                   --------------
 * So, for an APCString the caller gets back an APCHandle* pointing to the
 * location of the field in the class. APCString::fromHandle(APCHandle* handle)
 * will simply return the pointer itself as the handle sits at offset 0
 * (reinterpret_cast<APCString*>(handle)).
 * For an APCTypedValue, APCTypedValue::fromHandle(APCHandle* handle) would
 * return the handle pointer minus the offset (in words) of the handle
 * (reinterpret_cast<APCTypedValue*>(handle - 1)).
 * Normally the handle sits at position 0 so the call ends up being a no-op.
 * APCTypedValue is, at the moment, the only entity with the handle at a
 * non-zero offset and that is because the layout of APCTypedValue
 * and TypedValue have to be the same. The same layout allow for an important
 * optimization when an APCTypedValue is contained inside an APCArray. In such
 * cases the APCTypedalue is returned directly instead of creating another
 * TypedValue. That seems to show on perflab at the moment and we continue to
 * optimize that way.
 * APC entities normally contain their data after the handle. That allows us
 * to avoid multiple allocations and multiple dereferences.
 * The family of APC objects provide a pretty simple API to create and manage
 * the overall entity.
 * A set of static functions follow the given pattern (see APCXXX headers):
 * - MakeShared, creates and returns the pointer to APCHandle embedded
 * - fromHandle goes from the handle to the object instance (see above)
 * - MakeXXX (e.g. MakeArray, MakeString, ...) return the instance of the
 * PHP object to use by the request/thread
 * - a Destroy function may be present
 * the one common instance function is
 * - getHandle() which returns the pointer to the embedded APCHandle
 * - a private API can be provided. That private API can be used by other
 * objects that are aware of the APC model and can take advantage of that.
 * APCArray is a good example of that.
 *
 * It's important to point out that the type itself is not enough to
 * determine the instance behind the APCHandle. That is because both
 * APCObject and APCArray can have a serialized form that it is just a string
 * at the moment. Type and flags will tell you what the entity is.
 * This has not been an issue so far because things have been hiding behind
 * API when appropriate. It is however something we may want to revisit.
 */
struct APCHandle {

  /*
   * Create an instance of an APC object according to the type of source and
   * the various flags. This is the only entry point to create APC entities.
   */
  static APCHandle* Create(CVarRef source,
                           bool serialized,
                           bool inner = false,
                           bool unserializeObj = false);

  /*
   * Get an instance of the PHP object represented by this APCHandle. The
   * instance returned will be local to the request/thread that performed
   * the call.
   */
  Variant toLocal();

  //
  // Memory management API
  //
  void incRef() {
    assert(IS_REFCOUNTED_TYPE(m_type));
    ++m_count;
  }

  void decRef() {
    assert(m_count.load());
    if (m_count > 1) {
      assert(IS_REFCOUNTED_TYPE(m_type));
      --m_count;
    } else {
      assert(m_count.load() == 1);
      deleteShared();
    }
  }

  //
  // Type info API
  //
  bool is(DataType d) const { return m_type == d; }
  DataType getType() const { return m_type; }

  // TODO: those 2 methods should go back to private once we sort out
  //       the object creation story a bit better.
  //       The concurrent store tries to change the serialization format
  //       of an object on the fly and it needs those 2 methods.
  //       Right now that is still too intrusive but we need a bit more work
  //       before we can remove it
  //       TASK #3166547
  bool getIsObj() const { return m_flags & IsObj; }
  bool getObjAttempted() const { return m_flags & ObjAttempted; }

  //
  // Stats API
  //
  void getStats(APCHandleStats *stats) const;
  int32_t getSpaceUsage() const;

private:

  explicit APCHandle(DataType type)
      : m_type(type)
      , m_flags(0)
      , m_count(1) {
  }

  APCHandle(const APCHandle&) = delete;
  APCHandle& operator=(APCHandle const&) = delete;

  void deleteShared();

  static APCHandle* CreateSharedType(CVarRef source,
                                     bool serialized,
                                     bool inner,
                                     bool unserializeObj);

  bool shouldCache() const { return m_shouldCache; }
  void mustCache() { m_shouldCache = true; }

  friend struct APCTypedValue;
  friend struct APCString;
  friend struct APCObject;
  friend struct APCArray;

  const static uint8_t
    SerializedArray = (1<<0),
    IsPacked = (1<<1),
    IsObj = (1<<2),
    // APC object are usually in a serialized form. We try to make them
    // in a more performant format saving the array of properties to initialize
    // the object.
    // The following flag is set once that change of format is attempted so
    // that, if the format change is not possible (internal references), we do
    // not try to change the format over and over again.
    ObjAttempted = (1<<3);

  bool getSerializedArray() const { return m_flags & SerializedArray; }
  void setSerializedArray() { m_flags |= SerializedArray; }
  bool isPacked() const { return m_flags & IsPacked; }
  void setPacked() { m_flags |= IsPacked; }
  void setIsObj() { m_flags |= IsObj; }
  void setObjAttempted() { m_flags |= ObjAttempted; }

#if PACKED_TV
  bool m_shouldCache{false};
  DataType m_type;
  uint8_t m_flags;
  std::atomic<uint32_t> m_count;
#else
  DataType m_type;
  bool m_shouldCache{false};
  uint8_t m_flags;
  std::atomic<uint32_t> m_count;
#endif
};

///////////////////////////////////////////////////////////////////////////////

class APCHandleStats {
 public:
  int32_t dataSize;
  int32_t dataTotalSize;
  int32_t variantCount;

  void initStats() {
    variantCount = 0;
    dataSize = 0;
    dataTotalSize = 0;
  }

  APCHandleStats() {
    initStats();
  }

  void addChildStats(const APCHandleStats *childStats) {
    dataSize += childStats->dataSize;
    dataTotalSize += childStats->dataTotalSize;
    variantCount += childStats->variantCount;
  }

  void removeChildStats(const APCHandleStats *childStats) {
    dataSize -= childStats->dataSize;
    dataTotalSize -= childStats->dataTotalSize;
    variantCount -= childStats->variantCount;
  }
};

///////////////////////////////////////////////////////////////////////////////
}

#endif /* incl_HPHP_APC_HANDLE_H_ */
