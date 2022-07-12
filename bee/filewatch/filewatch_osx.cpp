#include <bee/filewatch/filewatch.h>
#include <bee/utility/unreachable.h>

namespace bee::filewatch {
    class task : public std::string {
    public:
        task(std::string && s)
            : std::string(std::move(s))
        {}
        task(std::string const& s)
            : std::string(s)
        {}
    };

    static void event_cb(ConstFSEventStreamRef streamRef,
        void* info,
        size_t numEvents,
        void* eventPaths,
        const FSEventStreamEventFlags eventFlags[],
        const FSEventStreamEventId eventIds[])
    {
        (void)streamRef;
        (void)eventIds;
        watch* self = (watch*)info;
        self->event_update((const char**)eventPaths, eventFlags, numEvents);
    }

    watch::watch()
        : m_notify()
        , m_gentask(0)
        , m_tasks()
        , m_stream(NULL)
    { }
    watch::~watch() {
        stop();
    }
    void watch::stop() {
        destroy_stream();
        m_tasks.clear();
    }

    bool watch::create_stream(CFArrayRef cf_paths) {
        if (m_stream) {
            return false;
        }
        FSEventStreamContext ctx = { 0 , this, NULL , NULL , NULL };

        FSEventStreamRef ref = 
            FSEventStreamCreate(NULL,
                &event_cb,
                &ctx,
                cf_paths,
                kFSEventStreamEventIdSinceNow,
                0.05,
                kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagFileEvents
            );
        assert(ref != NULL);
        m_fsevent_queue = dispatch_queue_create("fsevent_queue", NULL);
        FSEventStreamSetDispatchQueue(ref, m_fsevent_queue);
        if (!FSEventStreamStart(ref)) {
            FSEventStreamInvalidate(ref);
            FSEventStreamRelease(ref);
            return false;
        }
        m_stream = ref;
        return true;
    }

    void watch::destroy_stream() {
        if (!m_stream) {
            return;
        }
        FSEventStreamStop(m_stream);
        FSEventStreamInvalidate(m_stream);
        FSEventStreamRelease(m_stream);
        dispatch_release(m_fsevent_queue);
        m_stream = NULL;
    }

    taskid watch::add(const std::string& path) {
        taskid id = ++m_gentask;
        auto t = std::make_unique<task>(path);
        m_tasks.emplace(std::make_pair(id, std::move(t)));
        update_stream();
        return id;
    }

    bool watch::remove(taskid id) {
        m_tasks.erase(id);
        update_stream();
        return true;
    }

    void watch::update_stream() {
        destroy_stream();
        if (m_tasks.empty()) {
            return;
        }
        std::unique_ptr<CFStringRef[]> paths(new CFStringRef[m_tasks.size()]);
        size_t i = 0;
        for (auto& [id, t] : m_tasks) {
            paths[i] = CFStringCreateWithCString(NULL, t->c_str(), kCFStringEncodingUTF8);
            if (paths[i] == NULL) {
                while (i != 0) {
                    CFRelease(paths[--i]);
                }
                return;
            }
            i++;
        }
        CFArrayRef cf_paths = CFArrayCreate(NULL, (const void **)&paths[0], m_tasks.size(), NULL);
        if (create_stream(cf_paths)) {
            return;
        }
        CFRelease(cf_paths);
    }

    void watch::update() {
    }

    bool watch::select(notify& n) {
        if (m_notify.empty()) {
            return false;
        }
        n = m_notify.front();
        m_notify.pop();
        return true;
    }

    void watch::event_update(const char* paths[], const FSEventStreamEventFlags flags[], size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (flags[i] & (
                kFSEventStreamEventFlagItemCreated |
                kFSEventStreamEventFlagItemRemoved |
                kFSEventStreamEventFlagItemRenamed
            )) {
                m_notify.push({
                    notify_type::Rename, paths[i]
                });
            }
            else if (flags[i] & (
                kFSEventStreamEventFlagItemFinderInfoMod |
                kFSEventStreamEventFlagItemModified |
                kFSEventStreamEventFlagItemInodeMetaMod |
                kFSEventStreamEventFlagItemChangeOwner |
                kFSEventStreamEventFlagItemXattrMod
            )) {
                m_notify.push({
                    notify_type::Modify, paths[i]
                });
            }
        }
    }
}
