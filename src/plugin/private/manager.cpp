#pragma once

#include "plugin/manager.h"

#include "core/concurrency.h"
#include "core/file.h"
#include "core/library.h"
#include "core/map.h"
#include "core/string.h"
#include "core/uuid.h"

#include <cstring>
#include <utility>

namespace Plugin
{
	static const char* COPY_PATH = "plugin_tmp";

	struct PluginDesc
	{
		PluginDesc() = default;
		PluginDesc(const char* path, const char* libName)
		{
			fileName_.Printf("%s/%s", path, libName);
			tempFileName_.Printf("%s/%s", path, COPY_PATH);

			if(!Core::FileExists(tempFileName_.c_str()))
			{
				Core::FileCreateDir(tempFileName_.c_str());
			}
			tempFileName_.Appendf("/%s", libName);

			Reload();
		}

		~PluginDesc()
		{
			if(handle_)
			{
				Core::LibraryClose(handle_);
			}
		}

		bool Reload()
		{
			if(handle_)
			{
				Core::LibraryClose(handle_);
			}

			validPlugin_ = false;

			// First try to open + check for GetPlugin.
			{
				Core::LibHandle handle = Core::LibraryOpen(fileName_.data());
				if(!handle)
				{
					return false;
				}

				GetPluginFn getPlugin = (GetPluginFn)Core::LibrarySymbol(handle, "GetPlugin");
				if(!getPlugin)
				{
					Core::LibraryClose(handle);
					return false;
				}

				// Record modified timestamp.
				Core::FileStats(fileName_.data(), nullptr, &modifiedTimestamp_, nullptr);

				// Now close.
				Core::LibraryClose(handle);
			}

			// Make a copy to actually use to ensure the original file isn't locked when in use.
			if(!Core::FileCopy(fileName_.c_str(), tempFileName_.c_str()))
			{
				DBG_LOG("Failed to copy plugin library %s!\n", fileName_.data());
				return false;
			}

			handle_ = Core::LibraryOpen(tempFileName_.data());
			if(!handle_)
			{
				DBG_LOG("Unable to load plugin library %s!\n", fileName_.data());
				return false;
			}

			getPlugin_ = (GetPluginFn)Core::LibrarySymbol(handle_, "GetPlugin");
			if(!getPlugin_)
			{
				DBG_LOG("Unable to find symbol 'GetPlugin' for plugin library %s!\n", fileName_.data());
				return false;
			}


			if(!getPlugin_(&plugin_, Plugin::GetUUID()))
			{
				DBG_LOG("Unable to find UUID for plugin library %s!\n", fileName_.data());
				return false;
			}

			if(plugin_.systemVersion_ != PLUGIN_SYSTEM_VERSION)
			{
				DBG_LOG("System version mismatch for plugin library %s!\n", fileName_.data());
				return false;
			}

			validPlugin_ = true;
			return validPlugin_;
		}

		bool HasChanged() const
		{
			Core::FileTimestamp modifiedTimestamp;
			if(Core::FileStats(fileName_.data(), nullptr, &modifiedTimestamp, nullptr))
			{
				return modifiedTimestamp_ != modifiedTimestamp;
			}
			return false;
		}

		PluginDesc(const PluginDesc&) = delete;
		PluginDesc(PluginDesc&& other) = delete;
		void operator=(PluginDesc&& other) = delete;

		operator bool() const { return validPlugin_; }

		Core::String fileName_;
		Core::String tempFileName_;
		Core::FileTimestamp modifiedTimestamp_;
		Core::LibHandle handle_ = 0;
		GetPluginFn getPlugin_ = nullptr;
		Plugin plugin_;
		bool validPlugin_ = false;
	};

	struct ManagerImpl
	{
		Core::Map<Core::UUID, PluginDesc*> pluginDesc_;
		Core::Mutex mutex_;
	};

	ManagerImpl* impl_ = nullptr;

	void Manager::Initialize()
	{
		DBG_ASSERT(impl_ == nullptr);
		impl_ = new ManagerImpl();

		// Initial scan.
		Scan(".");
	}

	void Manager::Finalize()
	{
		DBG_ASSERT(impl_);
		for(auto pluginDescIt : impl_->pluginDesc_)
		{
			delete pluginDescIt.value;
		}
		impl_->pluginDesc_.clear();

		delete impl_;
		impl_ = nullptr;
	}

	bool Manager::IsInitialized() { return !!impl_; }

	i32 Manager::Scan(const char* path)
	{
		DBG_ASSERT(IsInitialized());
		Core::ScopedMutex lock(impl_->mutex_);
#if PLATFORM_WINDOWS
		const char* libExt = "dll";
#elif PLATFORM_LINUX || PLATFORM_OSX
		const char* libExt = "so";
#endif
		i32 foundLibs = Core::FileFindInPath(path, libExt, nullptr, 0);
		if(foundLibs)
		{
			Core::Vector<Core::FileInfo> fileInfos;
			fileInfos.resize(foundLibs);
			foundLibs = Core::FileFindInPath(path, libExt, fileInfos.data(), fileInfos.size());
			for(i32 i = 0; i < foundLibs; ++i)
			{
				const Core::FileInfo& fileInfo = fileInfos[i];
				PluginDesc* pluginDesc = new PluginDesc(path, fileInfo.fileName_);
				if(*pluginDesc)
				{
					pluginDesc->plugin_.fileName_ = pluginDesc->fileName_.data();
					pluginDesc->plugin_.fileUuid_ = Core::UUID(pluginDesc->plugin_.fileName_);

					if(impl_->pluginDesc_.find(pluginDesc->plugin_.fileUuid_) == nullptr)
					{
						impl_->pluginDesc_.insert(pluginDesc->plugin_.fileUuid_, pluginDesc);
					}
				}
				else
				{
					delete pluginDesc;
				}
			}
		}

		return impl_->pluginDesc_.size();
	}

	bool Manager::HasChanged(const Plugin& plugin)
	{
		DBG_ASSERT(IsInitialized());
		auto* val = impl_->pluginDesc_.find(plugin.fileUuid_);
		if(val != nullptr)
		{
			return (*val)->HasChanged();
		}
		return false;
	}

	bool Manager::Reload(Plugin& inOutPlugin)
	{
		DBG_ASSERT(IsInitialized());
		auto* val = impl_->pluginDesc_.find(inOutPlugin.fileUuid_);
		if(val != nullptr)
		{
			bool retVal = (*val)->Reload();
			if(retVal)
			{
				i32 num = GetPlugins(inOutPlugin.uuid_, &inOutPlugin, sizeof(Plugin), 1);
				if(num == 0)
					retVal = false;
			}
			else
			{
				// Erase plugin that failed to reload?
				impl_->pluginDesc_.erase(inOutPlugin.fileUuid_);
			}
			return retVal;
		}
		return false;
	}

	i32 Manager::GetPlugins(Core::UUID uuid, void* outPlugins, i32 pluginSize, i32 maxPlugins)
	{
		DBG_ASSERT(IsInitialized());
		Core::ScopedMutex lock(impl_->mutex_);

		i32 found = 0;

		if(outPlugins)
		{
			for(const auto& pair : impl_->pluginDesc_)
			{
				Plugin* plugin = (Plugin*)((u8*)outPlugins + (pluginSize * found));
				if(found < maxPlugins)
				{
					if(pair.value->getPlugin_(plugin, uuid))
					{
						plugin->fileUuid_ = pair.value->plugin_.fileUuid_;
						plugin->fileName_ = pair.value->fileName_.data();
						++found;
					}
				}
				else
					return found;
			}
		}
		else
		{
			for(const auto& pair : impl_->pluginDesc_)
			{
				if(pair.value->getPlugin_(nullptr, uuid))
					++found;
			}
		}

		return found;
	}


} // namespace Plugin
