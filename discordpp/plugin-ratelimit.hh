#pragma once

#include <algorithm>
#include <map>
#include <queue>
#include <regex>
#include <set>

#include <discordpp/log.hh>

namespace discordpp {
using route_t = std::size_t;

template <typename T> inline T stox(const std::string &s) {
    std::istringstream iss(s);
    T out;
    iss >> out;
    return out;
}

template <class BASE> class PluginRateLimit : public BASE, virtual BotStruct {
    struct CallInfo;
    using QueueByRoute = std::map<route_t, std::queue<sptr<CallInfo>>>;

  protected:
    void hasRateLimitPlugin() override{};

  public:
    // The Discord API *typically* limits to 5 calls so use that for unknown
    // buckets
    int defaultLimit = 5;
    bool unknownFirst = true;

    // Intercept calls
    virtual void doCall(sptr<RenderedCall> call) override {
        log::log(log::trace, [call](std::ostream *log) {
            *log << "Plugin: RateLimit: "
                 << "Intercepted " << *call->target;
            if (call->body) {
                *log << call->body;
            } else {
                *log << " with no body";
            }
            *log << '\n';
        });

        // Bundle the parameters together to pass them around easily
        // and calculate a hash of the route
        auto info = std::make_shared<CallInfo>(
            CallInfo{call, getLimitedRoute(*call->target)});

        log::log(log::trace, [info](std::ostream *log) {
            *log << "Hashes as " << info->route << '\n';
        });

        // Place the message in a bucket if possible,
        // otherwise place it on the global queue
        (route_to_bucket.count(info->route)
             ? buckets[route_to_bucket[info->route]].queues
             : queues)[info->route]
            .push(info);

        // Kickstart the send loop
        aioc->post([this] { do_some_work(); });
    }

  private:
    void do_some_work() {
        log::log(log::trace, [](std::ostream *log) {
            *log << "Check about doing work...";
        });
        // If already looping don't start a new loop
        if (writing || blocked) {
            log::log(log::trace,
                     [](std::ostream *log) { *log << " Not now."; });
            return;
        }
        writing = true;

        log::log(log::trace, [](std::ostream *log) { *log << " Can work..."; });

        // Find queue that needs to be sent next
        Bucket *next_bucket = nullptr;
        QueueByRoute *next_queues = nullptr;
        route_t next_route = 0;
        std::size_t min_remaining = defaultLimit;
        std::time_t min = std::numeric_limits<std::time_t>::max();
        for (auto &be : buckets) {
            if (route_to_bucket.count(gateway_route) &&
                route_to_bucket[gateway_route] == be.second.id &&
                be.second.queues.empty())
                continue;
            min_remaining =
                std::min(min_remaining,
                         be.second.remaining -
                             (be.second.transit.total() + transit.total()));
            if (be.second.remaining <=
                be.second.transit.total() + transit.total())
                continue;
            for (auto &qe : be.second.queues) {
                assert(!qe.second.empty() &&
                       "Encountered an empty queue in a bucket");
                auto created = qe.second.front()->created;
                if (created < min) {
                    min = created;
                    next_bucket = &be.second;
                    next_queues = &be.second.queues;
                    next_route = qe.first;
                }
            }
        }

        // `queues` may be empty
        for (auto &qe : queues) {
            assert(!qe.second.empty() && "Encountered an empty global queue");
            auto created = qe.second.front()->created;
            if (created < min || (unknownFirst && next_bucket != nullptr &&
                                  !transit.count(qe.first))) {
                min = created;
                next_bucket = nullptr;
                next_queues = &queues;
                next_route = qe.first;
            }
        }

        // Can we send a message?
        // If we found a next bucket, yes.
        // If the uncategorized queue isn't empty and there's no chance of
        // overflowing a bucket, yes.
        if (!next_bucket && (queues.empty() || min_remaining <= 0)) {
            log::log(log::trace,
                     [](std::ostream *log) { *log << " Nothing to do."; });
            writing = false;
            return;
        }

        log::log(log::trace, [](std::ostream *log) { *log << " Queueing."; });

        // Get the next call and delete its queue if empty
        auto next_queue = next_queues->find(next_route);
        auto info = next_queue->second.front();
        next_queue->second.pop();
        if (next_queue->second.empty()) {
            next_queues->erase(next_route);
        }

        log::log(log::trace, [info](std::ostream *log) {
            *log << "Plugin: RateLimit: "
                 << "Sending " << *info->call->target;
            if (info->call != nullptr) {
                *log << ' ' << *info->call->target;
                if (info->call->body) {
                    *log << info->call->body;
                } else {
                    *log << " with no body";
                }
            }
            *log << '\n';
        });

        // Do the call
        // Note: We are binding raw pointers and we must guarantee their
        // lifetimes
        // Note: `BASE::doCall` is used directly here as `Call::run` would
        // result in an infinite loop
        BASE::doCall(std::make_shared<RenderedCall>(
            info->call->method, info->call->target, info->call->type,
            info->call->body,
            std::make_shared<const handleWrite>(
                [this, route = next_route,
                 info](bool error) { // When the call is sent
                    info->writeFailed = error;

                    if (!error) {
                        // Mark the message as counting against rate
                        // limits while in transit
                        (route_to_bucket.count(route)
                             ? buckets[route_to_bucket[route]].transit
                             : transit)
                            .insert(route);
                    }
                    writing = false;

                    // Check if the cycle continues
                    aioc->post([this] { do_some_work(); });

                    // Run the user's onWrite callback
                    if (info->call->onWrite != nullptr) {
                        (*info->call->onWrite)(error);
                    }
                }),
            std::make_shared<const handleRead>([this, route = next_route,
                                                info](bool error,
                                                      const json &
                                                          msg) { // When Discord
                                                                 // replies
                // Get the current bucket
                auto *bucket = (route_to_bucket.count(route)
                                    ? &buckets[route_to_bucket[route]]
                                    : nullptr);

                // This message is no longer in transit
                if (!info->writeFailed) {
                    (bucket ? bucket->transit : transit).erase(route);
                }

                if (msg.contains("header") &&
                    (!error || msg["result"].get<int>() == 429)) {
                    auto &headers = msg["header"];
                    { // Find the new bucket and transfer other messages
                      // with the same route
                        auto new_id =
                            headers["X-RateLimit-Bucket"].get<std::string>();
                        if (!bucket || bucket->id != new_id) {
                            auto *old_bucket = bucket;

                            route_to_bucket[route] = new_id;
                            bucket = &buckets.emplace(new_id, Bucket{new_id})
                                          .first->second;

                            log::log(log::trace, [old_bucket,
                                                  bucket](std::ostream *log) {
                                *log << "Migrating from "
                                     << (old_bucket ? old_bucket->id : "global")
                                     << " to " << bucket->id << '\n';
                            });

                            bucket->queues.insert(
                                (old_bucket ? old_bucket->queues : queues)
                                    .extract(route));
                            (old_bucket ? old_bucket->transit : transit)
                                .move(bucket->transit, route);
                        }
                    }

                    // If ratelimited
                    if (msg["result"].get<int>() == 429) {
                        if (msg["body"]["global"]) { // If global, block all
                                                     // messages
                            blocked = true;
                            reset.reset();
                            reset = std::make_unique<asio::steady_timer>(*aioc);
                            reset->expires_after(std::chrono::seconds(
                                msg["body"]["retry_after"].get<int>()));
                            reset->async_wait([this](const error_code &e) {
                                // Don't reset the limit if the timer is
                                // cancelled
                                if (e) {
                                    return;
                                }
                                log::log(log::trace, [](std::ostream *log) {
                                    *log << "Global rate limit has "
                                            "elapsed.\n";
                                });
                                // Reset the limit
                                blocked = false;
                                // Kickstart the message sending process
                                aioc->post([this] { do_some_work(); });
                            });
                        } else { // Otherwise block this bucket
                            bucket->remaining = 0;
                            bucket->reset.reset();
                            bucket->reset =
                                std::make_unique<asio::steady_timer>(*aioc);
                            bucket->reset->expires_after(std::chrono::seconds(
                                msg["body"]["retry_after"].get<int>()));
                            bucket->reset->async_wait(
                                [this, owner = bucket](const error_code &e) {
                                    // Don't reset the limit if the timer is
                                    // cancelled
                                    if (e) {
                                        return;
                                    }
                                    log::log(log::trace,
                                             [owner](std::ostream *log) {
                                                 *log << "Limit reset for "
                                                      << owner->id << '\n';
                                             });
                                    // Reset the limit
                                    owner->remaining = owner->limit;
                                    // Kickstart the message sending process
                                    aioc->post([this] { do_some_work(); });
                                });
                        }

                        // Requeue this call
                        doCall(info->call);

                        return;
                    }

                    // Set the buckets new limits
                    bucket->limit = stox<std::size_t>(
                        headers["X-RateLimit-Limit"].get<std::string>());
                    bucket->remaining = std::min(
                        bucket->remaining,
                        stox<std::size_t>(headers["X-RateLimit-Remaining"]
                                              .get<std::string>()));

                    // Set a time for expiration of said limits
                    bucket->reset.reset();
                    bucket->reset = std::make_unique<asio::steady_timer>(*aioc);
                    bucket->reset->expires_after(
                        std::chrono::milliseconds(std::stoi(std::regex_replace(
                            headers["X-RateLimit-Reset-After"]
                                .get<std::string>(),
                            std::regex(R"([\D])"), ""))));
                    bucket->reset->async_wait(
                        [this, owner = bucket](const error_code &e) {
                            // Don't reset the limit if the timer is
                            // cancelled
                            if (e) {
                                return;
                            }
                            log::log(log::trace, [owner](std::ostream *log) {
                                *log << "Limit reset for " << owner->id << '\n';
                            });
                            // Reset the limit
                            owner->remaining = owner->limit;
                            // Kickstart the message sending process
                            aioc->post([this] { do_some_work(); });
                        });
                }

                // Run the user's onRead callback
                if (info->call->onRead != nullptr) {
                    (*info->call->onRead)(error, msg);
                }
            })));
    }

    struct CallInfo {
        sptr<RenderedCall> call;
        route_t route{};
        std::time_t created = std::time(nullptr);
        bool writeFailed = false;
    };

    template <typename T> struct CountedSet {
        [[nodiscard]] size_t total() const { return sum; }

        [[nodiscard]] size_t count(T t) const {
            return (map.count(t) ? map.at(t) : 0);
        }

        [[nodiscard]] bool empty() const { return !sum; }

        void insert(T t, size_t count = 1) {
            if (!count)
                return;

            auto entry = map.find(t);
            sum += count;
            if (entry != map.end()) {
                entry->second += count;
            } else {
                map.emplace(t, count);
            }
        }

        void erase(T t, size_t count = 1) {
            if (!count)
                return;

            auto entry = map.find(t);
            if (entry == map.end())
                throw std::out_of_range("Erased a key that doesn't exist");
            if (count > entry->second)
                throw std::out_of_range("Erased a key by more than its value");
            sum -= count;
            entry->second -= count;
            if (entry->second == 0) {
                map.erase(t);
            }
        }

        size_t clear(T t) {
            auto num = count(t);
            map.erase(t);
            return num;
        }

        void move(CountedSet<T> &other, T t) { other.insert(t, clear(t)); }

        void copy(CountedSet<T> &other, T t) { other.insert(t, count(t)); }

      private:
        size_t sum = 0;
        std::map<T, size_t> map;
    };

    struct Bucket {
        std::string id;

        QueueByRoute queues;
        CountedSet<route_t> transit;

        std::size_t limit = 5;
        std::size_t remaining = 4;
        std::unique_ptr<asio::steady_timer> reset;
    };

    bool writing = false;

    // These are for uncategorized calls
    QueueByRoute queues;
    CountedSet<route_t> transit;
    bool blocked = false;
    std::unique_ptr<asio::steady_timer> reset;

    std::map<route_t, std::string> route_to_bucket;
    std::map<std::string, Bucket> buckets;

    const route_t gateway_route = getLimitedRoute("/gateway/bot");
    route_t getLimitedRoute(const std::string &route) {
        std::ostringstream out;
        size_t next = route.find('/');
        std::string lastItem;
        while (next != std::string::npos) {
            size_t end = route.find('/', next + 1);

            std::string item = (end != std::string::npos
                                    ? route.substr(next + 1, end - next - 1)
                                    : route.substr(next + 1));

            out << "|";
            if (std::all_of(item.begin(), item.end(),
                            [](char c) { return std::isalpha(c); }) ||
                lastItem == "channels" || lastItem == "guilds" ||
                lastItem == "webhooks") {
                out << item;
            }
            lastItem = item;

            next = end;
        }
        return std::hash<std::string>{}(out.str());
    }
};
} // namespace discordpp
