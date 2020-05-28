#pragma once

#include <deque>
#include <map>
#include <set>

#include <discordpp/log.hh>

namespace discordpp{
	template<class BASE>
	class PluginRateLimit: public BASE, virtual BotStruct{
	public:
		// The Discord API *typically* limits to 5 calls so use that for unknown buckets
		int defaultLimit = 5;
		
		// Intercept calls
		virtual void call(
			sptr<const std::string> requestType,
			sptr<const std::string> targetURL,
			sptr<const json> body,
			sptr<const std::function<void()>> on_write,
			sptr<const std::function<void(const json)>> on_read
		) override{
			log::log(
				log::trace, [targetURL, body](std::ostream *log){
					*log << "Plugin: RateLimit: " << "Intercepted " << *targetURL << (body ? body->dump(4) : "{}")
					     << '\n';
				}
			);
			
			// Get rough route category based on keywords + guild, server, and webhook IDs
			std::size_t route = getLimitedRoute(*targetURL);
			log::log(
				log::trace, [route](std::ostream *log){
					*log << "\troute: " << route << '\n';
				}
			);
			// Is this route bucketed
			auto id = assigned.find(route);
			log::log(
				log::trace, [this, id](std::ostream *log){
					*log << "\tbucket: " << (id == assigned.end() ? "Uncategorised" : id->second) << '\n';
				}
			);
			
			// Place it on the global que if it isn't and the bucket queue if it is
			std::deque<sptr<Call>> &target = id == assigned.end() ? queue : buckets.find(id->second)->second.queue;
			log::log(
				log::trace, [this, target](std::ostream *log){
					*log << "\tbehind: " << target.size() << '\n';
				}
			);
			queue.push_back(
				std::make_shared<Call>(
					Call{
						requestType,
						targetURL,
						route,
						body,
						on_write,
						on_read
					}
				)
			);
			// Kickstart the send loop
			go();
		}
	
	private:
		void go(){
			// If already looping don't start a new loop
			if(active) return;
			active = true;
			
			log::log(
				log::trace, [](std::ostream *log){
					*log << "Starting RateLimit loop\n";
				}
			);
			
			// Identify the next call to send
			Bucket *nextBucket = getNext();
			log::log(
				log::trace, [this, nextBucket](std::ostream *log){
					if(nextBucket == nullptr){
						const bool bucketLimited = anyBucketLimited();
						*log << "Plugin: RateLimit: The next call is not from a bucket, "
						     << "the global queue is " << (queue.empty() ? "" : "not ") << "empty, "
						     << "there are " << transit.total() << '/' << defaultLimit << " messages in transit, "
						     << "and there is " << (bucketLimited ? "" : "not ") << "a limited bucket.\n"
						     << "Therefore " << (
						     	(queue.empty() || transit.total() >= defaultLimit || anyBucketLimited())
						     	? "the RateLimit loop is done for now."
						     	: "the next call is from the uncategorised queue."
						     	) << '\n';
					}else{
						*log << "Next call bucket: " << nextBucket->id << '\n'
						     << "\tt " << nextBucket->transit.total()
						     << " | r" << nextBucket->remaining
						     << " | l" << nextBucket->limit << '\n';
					}
				}
			);
			// If the next thing to run isn't bucketed AND (
			//     there's nothing to run OR
			//     there's a possibility of filling an unknown bucket (Could be less restrictive) OR
			//     the message could be assigned to a limited bucket
			// )
			// then we're done for now
			if(nextBucket == nullptr && (queue.empty() || transit.total() >= defaultLimit || anyBucketLimited())){
				active = false;
				return;
			}
			
			// Get the call to send
			std::deque<sptr<Call>> *nextQueue = nextBucket ? &nextBucket->queue : &queue;
			sptr<Call> call = nextQueue->front();
			nextQueue->pop_front();
			
			log::log(
				log::trace, [call](std::ostream *log){
					*log << "Plugin: RateLimit: " << "Calling " << call->targetURL
					     << (call->body ? call->body->dump(4) : "{}")
					     << '\n';
				}
			);
			
			// Calculate the route like before
			std::size_t route = getLimitedRoute(*call->targetURL);
			
			// Do the call
			BASE::call(
				call->requestType, call->targetURL, call->body,
				std::make_shared<std::function<void()>>(
					[this, nextBucket, call, route](){ // When the call is sent
						// Done with this active cycle
						active = false;
						
						// Count this call as on route to Discord
						transit.insert(route);
						if(nextBucket){
							nextBucket->transit.insert(route);
						}else{
							utransit.insert(route);
						}
						
						// Check if the cycle continues
						go();
						
						// Run the user's onWrite callback
						if(call->onWrite != nullptr){
							(*call->onWrite)();
						}
					}
				),
				std::make_shared<std::function<void(const json)>>(
					[this, call, route](const json &msg){ // When Discord replies
						// Find the existing bucket if there is one
						auto id = assigned.find(route);
						auto bucket = id == assigned.end() ? buckets.find(id->second) : buckets.end();
						
						// This message is no longer on route
						transit.erase(route);
						if(bucket != buckets.end()){
							bucket->second.transit.erase(route);
						}else{
							utransit.erase(route);
						}
						
						// Get the headers of the reply
						const json &headers = msg["header"];
						
						// The the bucket is new or changed
						if(id == assigned.end() || id->second != headers["X-RateLimit-Bucket"]){
							// Save a reference to the old bucket
							auto oldBucket = bucket;
							
							// Save the new bucket ID for this route
							assigned.emplace(route, headers["X-RateLimit-Bucket"]);
							id = assigned.find(route);
							
							// If it exists, get this route's new bucket
							bucket = buckets.find(headers["X-RateLimit-Bucket"]);
							// Otherwise create it
							if(bucket == buckets.end()){
								// Create the bucket with dummy data to overwrite
								buckets.emplace(
									headers["X-RateLimit-Bucket"].get<std::string>(),
									Bucket{
										headers["X-RateLimit-Bucket"].get<std::string>(), 0, 0,
										boost::asio::steady_timer(*aioc)
									}
								);
								bucket = buckets.find(headers["X-RateLimit-Bucket"]);
							}
							
							{ // Move queued calls with the same route to their new queue
								auto oldQueue = oldBucket != buckets.end() ? oldBucket->second.queue : queue;
								bool sort = false;
								for(auto c = oldQueue.begin(); c != oldQueue.end();){
									if((*c)->route == route){
										sort = true;
										bucket->second.queue.push_back(*c);
										c = oldQueue.erase(c);
									}else{
										c++;
									}
								}
								if(sort){
									std::sort(
										bucket->second.queue.begin(),
										bucket->second.queue.end(),
										[](auto a, auto b){
											return a->created < b->created;
										}
									);
								}
							}
							
							// Categorize the calls in transit
							if(oldBucket != buckets.end()){
								oldBucket->second.transit.move(bucket->second.transit, route);
							}else{
								utransit.move(bucket->second.transit, route);
							}
						}
						
						// Save the new bucket info
						bucket->second.limit = std::stoi(headers["X-RateLimit-Limit"].get<std::string>());
						bucket->second.remaining = std::stoi(headers["X-RateLimit-Remaining"].get<std::string>());
						
						// Reset the countdown timer for the new limit
						bucket->second.reset.expires_after(
							std::chrono::seconds(std::stoi(headers["X-RateLimit-Reset-After"].get<std::string>())));
						bucket->second.reset.async_wait(
							[this, owner = &bucket->second](const boost::system::error_code &e){
								// Don't reset the limit if the timer is cancelled
								if(e.failed()) return;
								log::log(log::trace, [owner](std::ostream *log){
									*log << "Limit reset for " << owner->id << '\n';
								});
								// Reset the limit
								owner->remaining = owner->limit;
								// Kickstart the message sending process
								go();
							}
						);
						
						// Run the user's onRead callback
						if(call->onRead != nullptr){
							(*call->onRead)(msg);
						}
					}
				)
			);
		}
		
		struct Call{
			sptr<const std::string> requestType;
			sptr<const std::string> targetURL;
			std::size_t route;
			sptr<const json> body;
			sptr<const std::function<void()>> onWrite;
			sptr<const std::function<void(const json)>> onRead;
			std::time_t created = std::time(nullptr);
		};
		
		template<typename T>
		struct CountedSet{
			unsigned int total() const{
				return sum;
			}
			
			unsigned int count(T t) const{
				auto entry = map.find(t);
				if(entry != map.end()){
					return entry->second;
				}else{
					return 0;
				}
			}
			
			bool empty() const{
				return !sum;
			}
			
			void insert(T t, unsigned int count = 1){
				if(count){
					auto entry = map.find(t);
					sum += count;
					if(entry != map.end()){
						entry->second++;
					}else{
						map.emplace(t, 1);
					}
				}
			}
			
			void erase(T t, unsigned int count = 1){
				if(count){
					auto entry = map.find(t);
					if(entry != map.end()){
						sum -= count;
						entry->second -= count;
						if(entry->second == 0){
							map.erase(t);
						}
					}
				}
			}
			
			int clear(T t){
				int num = count(t);
				erase(t, num);
				return num;
			}
			
			void move(CountedSet &other, T t){
				other.insert(t, clear(t));
			}
			
			void copy(CountedSet &other, T t){
				other.insert(t, count(t));
			}
		
		private:
			unsigned int sum = 0;
			std::map<T, unsigned int> map;
		};
		
		struct Bucket{
			std::string id;
			
			int limit;
			int remaining;
			//int reset;
			//int resetAfter;
			boost::asio::steady_timer reset;
			
			CountedSet<std::size_t> transit;
			std::deque<sptr < Call>> queue;
			
			
			void insert(std::size_t hash, unsigned int transitCount){
				transit.insert(hash, transitCount);
			}
			
			unsigned int remove(std::size_t hash){
				int transitCount = transit.count(hash);
				transit.remove(hash, transitCount);
				return transitCount;
			}
		};
		
		CountedSet<std::size_t> transit;
		CountedSet<std::size_t> utransit;
		std::deque<sptr < Call>> queue;
		
		bool active = false;
		
		std::map<std::size_t, std::string> assigned;
		std::map<std::string, Bucket> buckets;
		
		Bucket *getNext(){
			Bucket *next = nullptr;
			std::time_t first = std::numeric_limits<std::time_t>::max();
			for(auto &bucket : buckets){
				if(
					!bucket.second.queue.empty() &&
					bucket.second.remaining - bucket.second.transit.total() > 0 &&
					first > bucket.second.queue.front()->created
				){
					next = &bucket.second;
					first = next->queue.front()->created;
				}
			}
			return next;
		}
		
		bool anyBucketLimited(){
			for(auto &bucket : buckets){
				if(bucket.second.remaining - bucket.second.transit.total() - utransit.total() <= 0){
					return true;
				}
			}
			return false;
		}
		
		std::size_t getLimitedRoute(const std::string &route){
			std::ostringstream out;
			size_t last = route.find('\\');
			size_t next = route.find('\\', last + 1);
			std::string lastItem;
			while(last != std::string::npos){
				std::string item = route.substr(last, next);
				if(std::find_if_not(item.begin(), item.end(), [](char c){return isalpha(c);}) == item.end() ||
				   lastItem == "channels" || lastItem == "guilds" || lastItem == "webhooks"){
					out << item;
				}else{
					out << "|";
				}
				lastItem = item;
			}
			return std::hash<std::string>{}(out.str());
		}
	};
}