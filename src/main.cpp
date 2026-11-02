// UIMessageQueue::AddMessage takes the next of its 64 pooled messages by reading the pool count (+0x378), assigns the
// message's menu name - a BSFixedString assignment: acquire the new name, release the one the slot held before - and
// then increments the count with a plain inc. Nothing is locked, yet the game and plugins queue UI messages from job
// threads as well as the main thread. Two threads inside at once take the same slot and both release its old name: a
// menu name's string-pool entry ("Fader Menu", "HUD Menu", ...) loses one reference each time, until the pool frees it
// while the UI still uses it. The next reader of that name crashes on garbage (its length or text), in whatever code
// reads it. ProcessCommands, which empties the pool after each round of UI processing, also resets the count with no
// lock. This plugin runs both functions under one lock.
// The two functions are the same code in 1.5.97, 1.6.1170 and 1.7.104; their prologues and the pool-count instruction
// are checked at load. An entry that another plugin already detoured (a 14-byte jmp [rip] or a 5-byte jmp) is chained:
// that detour then runs inside the lock. Anything else stays unpatched and says so in UIMessageQueueRaceFix.log.

namespace
{
	const REL::RelocationID kAddMessage{ 13530, 13631 };
	const REL::RelocationID kProcessCommands{ 80059, 82167 };

	struct Target
	{
		REL::RelocationID         id;
		std::vector<std::uint8_t> prologue;  // copied to the trampoline: no rip-relative bytes
		std::size_t               markerAt;
		std::vector<std::uint8_t> marker;    // the pool-count instruction
		std::string_view          name;
	};

	const Target kAdd{
		kAddMessage,
		{ 0x40, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF },  // push rsi/rdi/r14; sub rsp,30h; mov [rsp+20h],-2
		0x28,
		{ 0x8B, 0x81, 0x78, 0x03, 0x00, 0x00 },  // mov eax,[rcx+378h]
		"UIMessageQueue::AddMessage"sv
	};
	const Target kProcess{
		kProcessCommands,
		{ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20 },  // mov [rsp+10h],rbx; mov [rsp+18h],rbp; push rsi; sub rsp,20h
		0x16,
		{ 0x39, 0x99, 0x78, 0x03, 0x00, 0x00 },  // cmp [rcx+378h],ebx
		"UIMessageQueue::ProcessCommands"sv
	};

	using AddMessage_t = void (*)(RE::UIMessageQueue*, const RE::BSFixedString&, RE::UI_MESSAGE_TYPE, RE::IUIMessageData*);
	using ProcessCommands_t = void (*)(RE::UIMessageQueue*);
	AddMessage_t      g_addMessage = nullptr;
	ProcessCommands_t g_processCommands = nullptr;

	std::recursive_mutex       g_lock;
	std::atomic<std::uint32_t> g_waits{ 0 };

	std::unique_lock<std::recursive_mutex> Lock(std::string_view a_what)
	{
		std::unique_lock lock{ g_lock, std::try_to_lock };
		if (!lock.owns_lock()) {
			const auto n = ++g_waits;
			if (n <= 20 || n % 1000 == 0) {  // the first 20, then every 1000th: a busy UI can't grow the log
				logger::info("{} on thread {} waited for another thread's call (#{})"sv, a_what, ::GetCurrentThreadId(), n);
			}
			lock.lock();
		}
		return lock;
	}

	void AddMessage(RE::UIMessageQueue* a_queue, const RE::BSFixedString& a_menu, RE::UI_MESSAGE_TYPE a_type, RE::IUIMessageData* a_data)
	{
		const auto lock = Lock("AddMessage"sv);
		g_addMessage(a_queue, a_menu, a_type, a_data);
	}

	void ProcessCommands(RE::UIMessageQueue* a_queue)
	{
		const auto lock = Lock("ProcessCommands"sv);
		g_processCommands(a_queue);
	}

	enum class Entry
	{
		kOriginal,
		kJmpAbs,   // FF 25 00000000 <abs64>
		kJmpRel,   // E9 <rel32>
		kUnknown
	};

	const std::uint8_t* Code(const Target& a_t) { return reinterpret_cast<const std::uint8_t*>(a_t.id.address()); }

	Entry Classify(const Target& a_t)
	{
		const auto* c = Code(a_t);
		if (std::equal(a_t.prologue.begin(), a_t.prologue.end(), c)) {
			return std::equal(a_t.marker.begin(), a_t.marker.end(), c + a_t.markerAt) ? Entry::kOriginal : Entry::kUnknown;
		}
		if (c[0] == 0xFF && c[1] == 0x25 && !c[2] && !c[3] && !c[4] && !c[5]) {
			return Entry::kJmpAbs;
		}
		return c[0] == 0xE9 ? Entry::kJmpRel : Entry::kUnknown;
	}

	std::string_view Describe(Entry a_e)
	{
		switch (a_e) {
		case Entry::kOriginal:
			return "original code"sv;
		case Entry::kJmpAbs:
		case Entry::kJmpRel:
			return "chained to the detour already at its entry"sv;
		default:
			return "unknown code"sv;
		}
	}

	// writes "jmp [rip+0]; <abs64 a_to>" (14 bytes)
	void AbsJump(std::uint8_t* a_at, std::uintptr_t a_to)
	{
		a_at[0] = 0xFF;
		a_at[1] = 0x25;
		std::memset(a_at + 2, 0, 4);
		std::memcpy(a_at + 6, &a_to, 8);
	}

	// Routes the entry to a_hook; returns what a_hook must call next. The next pointer is complete before the entry
	// changes.
	template <class F>
	void Detour(const Target& a_t, Entry a_e, F a_hook, F& a_next)
	{
		auto&      trampoline = SKSE::GetTrampoline();
		const auto entry = a_t.id.address();
		const auto hook = reinterpret_cast<std::uintptr_t>(a_hook);
		switch (a_e) {
		case Entry::kOriginal:
			{
				const auto len = a_t.prologue.size();
				auto*      tramp = static_cast<std::uint8_t*>(trampoline.allocate(len + 14));
				std::memcpy(tramp, a_t.prologue.data(), len);
				AbsJump(tramp + len, entry + len);
				a_next = reinterpret_cast<F>(tramp);
				std::vector<std::uint8_t> patch(len, 0x90);
				AbsJump(patch.data(), hook);
				REL::safe_write(entry, patch.data(), patch.size());
				break;
			}
		case Entry::kJmpAbs:
			{
				std::uintptr_t prev = 0;
				std::memcpy(&prev, Code(a_t) + 6, 8);
				a_next = reinterpret_cast<F>(prev);
				REL::safe_write(entry + 6, &hook, 8);
				break;
			}
		case Entry::kJmpRel:
			{
				const auto prev = entry + 5 + *reinterpret_cast<const std::int32_t*>(Code(a_t) + 1);
				a_next = reinterpret_cast<F>(prev);
				auto* stub = static_cast<std::uint8_t*>(trampoline.allocate(14));  // within 2 GB of the exe
				AbsJump(stub, hook);
				const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(stub) - static_cast<std::intptr_t>(entry + 5));
				REL::safe_write(entry + 1, &rel, 4);
				break;
			}
		default:
			break;
		}
	}
}

SKSEPluginInfo(
	.Version = Plugin::VERSION,
	.Name = Plugin::NAME,
	.Author = "Yuof"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .log = true });
	logger::info("{} v{} on {}"sv, Plugin::NAME, Plugin::VERSION.string(), REL::Module::get().version().string());
	// SE and AE code was read; VR was not (no VR executable to check its ids and code against)
	if (REL::Module::IsVR()) {
		logger::error("VR is not supported: inactive"sv);
		return true;
	}
	const auto add = Classify(kAdd);
	const auto process = Classify(kProcess);
	for (const auto& [t, e] : { std::pair{ &kAdd, add }, std::pair{ &kProcess, process } }) {
		if (e == Entry::kUnknown) {
			logger::error("self-check failed ({}: unexpected code at its entry): inactive, nothing patched"sv, t->name);
			return true;
		}
	}
	SKSE::AllocTrampoline(64);
	Detour(kAdd, add, &AddMessage, g_addMessage);
	Detour(kProcess, process, &ProcessCommands, g_processCommands);
	logger::info("patched {} ({}) and {} ({}): both run under one lock"sv, kAdd.name, Describe(add), kProcess.name, Describe(process));
	return true;
}
