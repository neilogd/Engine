#include "core/concurrency.h"
#include "core/debug.h"
#include "core/string.h"

#include <utility>

#if !CODE_INLINE
#include "core/private/concurrency.inl"
#endif

#if PLATFORM_WINDOWS
#include "core/os.h"

#include "Remotery.h"

namespace Core
{
	i32 GetNumLogicalCores()
	{
		SYSTEM_INFO sysInfo = {};
		::GetSystemInfo(&sysInfo);
		return sysInfo.dwNumberOfProcessors;
	}

	i32 GetNumPhysicalCores()
	{
		Core::Array<SYSTEM_LOGICAL_PROCESSOR_INFORMATION, 256> info = {};
		const i32 size = sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
		DWORD len = info.size() * size;
		::GetLogicalProcessorInformation(info.data(), &len);
		const i32 numInfos = len / size;
		i32 numCores = 0;
		for(i32 idx = 0; idx < numInfos; ++idx)
		{
			if(info[idx].Relationship == RelationProcessorCore)
			{
				++numCores;
			}
		}
		return numCores;
	}

	u64 GetPhysicalCoreAffinityMask(i32 core)
	{
		Core::Array<SYSTEM_LOGICAL_PROCESSOR_INFORMATION, 256> info = {};
		const i32 size = sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
		DWORD len = info.size() * size;
		::GetLogicalProcessorInformation(info.data(), &len);
		const i32 numInfos = len / size;
		i32 numCores = 0;
		for(i32 idx = 0; idx < numInfos; ++idx)
		{
			if(info[idx].Relationship == RelationProcessorCore)
			{
				if(numCores == core)
				{
					return info[idx].ProcessorMask;
				}
				++numCores;
			}
		}
		return 0;
	}

	struct ThreadImpl
	{
		DWORD threadId_ = 0;
		HANDLE threadHandle_ = 0;
		Thread::EntryPointFunc entryPointFunc_ = nullptr;
		void* userData_ = nullptr;
#if !defined(_RELEASE)
		Core::String debugName_;
#endif
	};

	static DWORD WINAPI ThreadEntryPoint(LPVOID lpThreadParameter)
	{
		auto* impl = reinterpret_cast<ThreadImpl*>(lpThreadParameter);

#if !defined(_RELEASE)
		if(IsDebuggerAttached() && impl->debugName_.size() > 0)
		{
#pragma pack(push, 8)
			typedef struct tagTHREADNAME_INFO
			{
				DWORD dwType;     /* must be 0x1000 */
				LPCSTR szName;    /* pointer to name (in user addr space) */
				DWORD dwThreadID; /* thread ID (-1=caller thread) */
				DWORD dwFlags;    /* reserved for future use, must be zero */
			} THREADNAME_INFO;
#pragma pack(pop)
			THREADNAME_INFO info;
			memset(&info, 0, sizeof(info));
			info.dwType = 0x1000;
			info.szName = impl->debugName_.c_str();
			info.dwThreadID = (DWORD)-1;
			info.dwFlags = 0;
			::RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG), (const ULONG_PTR*)&info);
		}

		if(impl->debugName_.size() > 0)
		{
			rmt_SetCurrentThreadName(impl->debugName_.c_str());
			rmt_ScopedCPUSample(ThreadBegin, RMTSF_None);
		}
#endif
		return impl->entryPointFunc_(impl->userData_);
	}

	Thread::Thread(EntryPointFunc entryPointFunc, void* userData, i32 stackSize, const char* debugName)
	{
		DBG_ASSERT(entryPointFunc);
		DWORD creationFlags = 0;
		impl_ = new ThreadImpl();
		impl_->entryPointFunc_ = entryPointFunc;
		impl_->userData_ = userData;
		impl_->threadHandle_ =
		    ::CreateThread(nullptr, stackSize, ThreadEntryPoint, impl_, creationFlags, &impl_->threadId_);
#if !defined(_RELEASE)
		impl_->debugName_ = debugName;
		debugName_ = impl_->debugName_.c_str();
#endif
	}

	Thread::~Thread()
	{
		if(impl_)
		{
			Join();
		}
	}

	Thread::Thread(Thread&& other)
	{
		using std::swap;
		swap(impl_, other.impl_);
#if !defined(_RELEASE)
		swap(debugName_, other.debugName_);
#endif
	}

	Thread& Thread::operator=(Thread&& other)
	{
		using std::swap;
		swap(impl_, other.impl_);
#if !defined(_RELEASE)
		swap(debugName_, other.debugName_);
#endif
		return *this;
	}

	u64 Thread::SetAffinity(u64 mask) { return ::SetThreadAffinityMask(impl_->threadHandle_, mask); }

	i32 Thread::Join()
	{
		if(impl_)
		{
			::WaitForSingleObject(impl_->threadHandle_, INFINITE);
			DWORD exitCode = 0;
			BOOL success = ::GetExitCodeThread(impl_->threadHandle_, &exitCode);
			DBG_ASSERT(success);
			::CloseHandle(impl_->threadHandle_);
			delete impl_;
			impl_ = nullptr;
			return exitCode;
		}
		return 0;
	}

	/// Use Fiber Local Storage to store current fiber.
	static FLS thisFiber_;

	struct FiberImpl
	{
		static const u64 SENTINAL = 0x11207CE82F00AA5ALL;
		u64 sentinal_ = SENTINAL;
		Fiber* parent_ = nullptr;
		void* fiber_ = nullptr;
		void* exitFiber_ = nullptr;
		Fiber::EntryPointFunc entryPointFunc_ = nullptr;
		void* userData_ = nullptr;
#if !defined(_RELEASE)
		Core::String debugName_;
#endif
	};

	static void __stdcall FiberEntryPoint(LPVOID lpParameter)
	{
		auto* impl = reinterpret_cast<FiberImpl*>(lpParameter);
		thisFiber_.Set(impl);

		impl->entryPointFunc_(impl->userData_);
		DBG_ASSERT(impl->exitFiber_);
		::SwitchToFiber(impl->exitFiber_);
	}

	Fiber::Fiber(EntryPointFunc entryPointFunc, void* userData, i32 stackSize, const char* debugName)
	{
		DBG_ASSERT(entryPointFunc);
		impl_ = new FiberImpl();
		impl_->parent_ = this;
		impl_->entryPointFunc_ = entryPointFunc;
		impl_->userData_ = userData;
		impl_->fiber_ = ::CreateFiber(stackSize, FiberEntryPoint, impl_);
#if !defined(_RELEASE)
		impl_->debugName_ = debugName_;
		debugName_ = impl_->debugName_.c_str();
#endif
		DBG_ASSERT_MSG(impl_->fiber_, "Unable to create fiber.");
		if(impl_->fiber_ == nullptr)
		{
			delete impl_;
			impl_ = nullptr;
		}
	}

	Fiber::Fiber(ThisThread, const char* debugName)
#if !defined(_RELEASE)
	    : debugName_(debugName)
#endif
	{
		impl_ = new FiberImpl();
		impl_->parent_ = this;
		impl_->entryPointFunc_ = nullptr;
		impl_->userData_ = nullptr;
		impl_->fiber_ = ::ConvertThreadToFiber(impl_);
#if !defined(_RELEASE)
		impl_->debugName_ = debugName_;
#endif
		DBG_ASSERT_MSG(impl_->fiber_, "Unable to create fiber. Is there already one for this thread?");
		if(impl_->fiber_ == nullptr)
		{
			delete impl_;
			impl_ = nullptr;
		}
	}

	Fiber::~Fiber()
	{
		if(impl_)
		{
			if(impl_->entryPointFunc_)
			{
				::DeleteFiber(impl_->fiber_);
			}
			else
			{
				::ConvertFiberToThread();
			}
			delete impl_;
		}
	}

	Fiber::Fiber(Fiber&& other)
	{
		using std::swap;
		swap(impl_, other.impl_);
#if !defined(_RELEASE)
		swap(debugName_, other.debugName_);
#endif
		impl_->parent_ = this;
	}

	Fiber& Fiber::operator=(Fiber&& other)
	{
		using std::swap;
		swap(impl_, other.impl_);
#if !defined(_RELEASE)
		swap(debugName_, other.debugName_);
#endif
		impl_->parent_ = this;
		return *this;
	}

	void Fiber::SwitchTo()
	{
		DBG_ASSERT(impl_);
		DBG_ASSERT(impl_->parent_ == this);
		DBG_ASSERT(::GetCurrentFiber() != nullptr);
		if(impl_)
		{
			DBG_ASSERT(::GetCurrentFiber() != impl_->fiber_);
			void* lastExitFiber = impl_->exitFiber_;
			impl_->exitFiber_ = impl_->entryPointFunc_ ? ::GetCurrentFiber() : nullptr;
			::SwitchToFiber(impl_->fiber_);
			impl_->exitFiber_ = lastExitFiber;
		}
	}

	void* Fiber::GetUserData() const
	{
		DBG_ASSERT(impl_);
		DBG_ASSERT(impl_->parent_ == this);
		return impl_->userData_;
	}

	Fiber* Fiber::GetCurrentFiber()
	{
		auto* impl = (FiberImpl*)thisFiber_.Get();
		if(impl)
			return impl->parent_;
		return nullptr;
	}

	struct SemaphoreImpl
	{
		HANDLE handle_;
#if !defined(_RELEASE)
		Core::String debugName_;
#endif
	};

	struct SemaphoreImpl* Semaphore::Get() { return reinterpret_cast<SemaphoreImpl*>(&implData_[0]); }

	Semaphore::Semaphore(i32 initialCount, i32 maximumCount, const char* debugName)
	{
		DBG_ASSERT(initialCount >= 0);
		DBG_ASSERT(maximumCount >= 0);

		new(implData_) SemaphoreImpl();

		// NOTE: Don't set debug name on semaphore. If 2 names are the same, they'll reference the same event.
		Get()->handle_ = ::CreateSemaphore(nullptr, initialCount, maximumCount, nullptr);
#if !defined(_RELEASE)
		Get()->debugName_ = debugName;
		debugName_ = Get()->debugName_.c_str();
#endif
	}

	Semaphore::~Semaphore()
	{
		::CloseHandle(Get()->handle_);
		Get()->~SemaphoreImpl();
	}

	Semaphore::Semaphore(Semaphore&& other)
	{
		using std::swap;
		swap(implData_, other.implData_);
#if !defined(_RELEASE)
		swap(debugName_, other.debugName_);
#endif
	}

	bool Semaphore::Wait(i32 timeout)
	{
		DBG_ASSERT(Get());
		return (::WaitForSingleObject(Get()->handle_, timeout) == WAIT_OBJECT_0);
	}

	bool Semaphore::Signal(i32 count)
	{
		DBG_ASSERT(Get());
		return !!::ReleaseSemaphore(Get()->handle_, count, nullptr);
	}

	SpinLock::SpinLock() {}

	SpinLock::~SpinLock() { DBG_ASSERT(count_ == 0); }

	void SpinLock::Lock()
	{
		while(Core::AtomicCmpExchgAcq(&count_, 1, 0) == 1)
		{
			Core::YieldCPU();
		}
	}

	bool SpinLock::TryLock() { return (Core::AtomicCmpExchgAcq(&count_, 1, 0) == 0); }

	void SpinLock::Unlock()
	{
		i32 count = Core::AtomicExchg(&count_, 0);
		DBG_ASSERT(count == 1);
	}

	struct MutexImpl
	{
		CRITICAL_SECTION critSec_;
		HANDLE lockThread_ = nullptr;
		volatile i32 lockCount_ = 0;
	};

	struct MutexImpl* Mutex::Get() { return reinterpret_cast<MutexImpl*>(&implData_[0]); }

	Mutex::Mutex()
	{
		static_assert(sizeof(MutexImpl) <= sizeof(implData_), "implData_ too small for MutexImpl!");
		new(implData_) MutexImpl();
		::InitializeCriticalSection(&Get()->critSec_);
	}

	Mutex::~Mutex()
	{
		::DeleteCriticalSection(&Get()->critSec_);
		Get()->~MutexImpl();
	}

	Mutex::Mutex(Mutex&& other)
	{
		using std::swap;
		other.Lock();
		std::swap(implData_, other.implData_);
		Unlock();
	}

	Mutex& Mutex::operator=(Mutex&& other)
	{
		using std::swap;
		other.Lock();
		std::swap(implData_, other.implData_);
		Unlock();
		return *this;
	}

	void Mutex::Lock()
	{
		DBG_ASSERT(Get());
		::EnterCriticalSection(&Get()->critSec_);
		if(AtomicInc(&Get()->lockCount_) == 1)
			Get()->lockThread_ = ::GetCurrentThread();
	}

	bool Mutex::TryLock()
	{
		DBG_ASSERT(Get());
		if(!!::TryEnterCriticalSection(&Get()->critSec_))
		{
			if(AtomicInc(&Get()->lockCount_) == 1)
				Get()->lockThread_ = ::GetCurrentThread();
			return true;
		}
		return false;
	}

	void Mutex::Unlock()
	{
		DBG_ASSERT(Get());
		DBG_ASSERT(Get()->lockThread_ == ::GetCurrentThread());
		if(AtomicDec(&Get()->lockCount_) == 0)
			Get()->lockThread_ = nullptr;
		::LeaveCriticalSection(&Get()->critSec_);
	}

	struct RWLockImpl
	{
		SRWLOCK srwLock_ = SRWLOCK_INIT;
	};

	struct RWLockImpl* RWLock::Get() { return reinterpret_cast<RWLockImpl*>(&implData_[0]); }
	struct RWLockImpl* RWLock::Get() const { return reinterpret_cast<RWLockImpl*>(&implData_[0]); }

	RWLock::RWLock()
	{
		static_assert(sizeof(RWLockImpl) <= sizeof(implData_), "implData_ too small for RWLockImpl!");
		new(implData_) RWLockImpl;
	}

	RWLock::~RWLock()
	{
#if !defined(_RELEASE)
		if(!::TryAcquireSRWLockExclusive(&(Get()->srwLock_)))
			DBG_ASSERT(false);
#endif
		Get()->~RWLockImpl();
	}

	RWLock::RWLock(RWLock&& other)
	{
		using std::swap;
		std::swap(implData_, other.implData_);
	}

	RWLock& RWLock::operator=(RWLock&& other)
	{
		using std::swap;
		std::swap(implData_, other.implData_);
		return *this;
	}

	void RWLock::BeginRead() const { ::AcquireSRWLockShared(&Get()->srwLock_); }

	void RWLock::EndRead() const { ::ReleaseSRWLockShared(&Get()->srwLock_); }

	void RWLock::BeginWrite() { ::AcquireSRWLockExclusive(&Get()->srwLock_); }

	void RWLock::EndWrite() { ::ReleaseSRWLockExclusive(&Get()->srwLock_); }

	struct TLSImpl
	{
		DWORD handle_ = 0;
	};

	TLS::TLS() { handle_ = TlsAlloc(); }

	TLS::~TLS() { ::TlsFree(handle_); }

	bool TLS::Set(void* data)
	{
		DBG_ASSERT(handle_ >= 0);
		return !!::TlsSetValue(handle_, data);
	}

	void* TLS::Get() const
	{
		DBG_ASSERT(handle_ >= 0);
		return ::TlsGetValue(handle_);
	}


	FLS::FLS() { handle_ = FlsAlloc(nullptr); }

	FLS::~FLS() { ::FlsFree(handle_); }

	bool FLS::Set(void* data)
	{
		DBG_ASSERT(handle_ >= 0);
		return !!::FlsSetValue(handle_, data);
	}

	void* FLS::Get() const
	{
		DBG_ASSERT(handle_ >= 0);
		return ::FlsGetValue(handle_);
	}

} // namespace Core
#else
#error "Not implemented for platform!""
#endif
