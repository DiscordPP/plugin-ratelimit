#pragma once

#include <deque>
#include <map>
#include <set>

namespace discordpp{
	template<class BASE>
	class PluginRateLimit: public BASE, virtual BotStruct{
	public:
		int defaultLimit = 5;
		
		virtual void call(
			sptr<const std::string> requestType,
			sptr<const std::string> targetURL,
			sptr<const json> body,
			sptr<const std::function<void()>> on_write,
			sptr<const std::function<void(const json)>> on_read
		) override{
			std::size_t route = getLimitedRoute(*targetURL);
			auto id = assigned.find(route);
			std::deque<sptr<Call>> &target = id == assigned.end() ? queue : buckets.find(id->second)->second.queue;
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
			go();
		}
	
	private:
		void go(){
			if(active) return;
			active = true;
			
			Bucket *nextBucket = getNext();
			if(nextBucket == nullptr && (queue.empty() || transit.total() >= defaultLimit || anyBucketLimited())){
				active = false;
				return;
			}
			
			std::deque<sptr<Call>> &nextQueue = nextBucket ? nextBucket->queue : queue;
			sptr<Call> call = nextQueue.front();
			nextQueue.pop_back();
			
			std::size_t route = getLimitedRoute(*call->targetURL);
			
			BASE::call(
				call->requestType, call->targetURL, call->body,
				std::make_shared<std::function<void()>>(
					[this, nextBucket, call, route](){
						active = false;
						
						transit.insert(route);
						if(nextBucket){
							nextBucket->transit.insert(route);
						}
						
						go();
						
						if(call->onWrite != nullptr){
							(*call->onWrite)();
						}
						std::cout << "on_write" << std::endl;
					}
				),
				std::make_shared<std::function<void(const json)>>(
					[this, call, route](const json &msg){
						auto id = assigned.find(route);
						auto bucket = id == assigned.end() ? buckets.find(id->second) : buckets.end();
						
						transit.erase(route);
						if(bucket != buckets.end()){
							bucket->second.transit.erase(route);
						}
						
						const json &headers = msg["header"];
						
						if(id == assigned.end() || id->second != headers["X-RateLimit-Bucket"]){
							auto oldBucket = bucket;
							
							assigned.emplace(route, headers["X-RateLimit-Bucket"]);
							id = assigned.find(route);
							
							bucket = buckets.find(headers["X-RateLimit-Bucket"]);
							if(bucket == buckets.end()){
								buckets.emplace(
									headers["X-RateLimit-Bucket"].get<std::string>(),
									Bucket{0, 0, boost::asio::steady_timer(*aioc)}
								);
								bucket = buckets.find(headers["X-RateLimit-Bucket"]);
							}
							
							{
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
							
							if(oldBucket != buckets.end()){
								bucket->second.transit.move(bucket->second.transit, route);
							}
						}
						
						bucket->second.limit = headers["X-RateLimit-Limit"];
						bucket->second.remaining = headers["X-RateLimit-Remaining"];
						bucket->second.reset.expires_after(std::chrono::seconds(std::stoi(headers["X-RateLimit-Remaining"].get<std::string>())));
						bucket->second.reset.async_wait([this, owner = &bucket->second](const boost::system::error_code& e){
							if(e.failed()) return;
							owner->remaining = owner->limit;
							go();
						});
						
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
		std::deque<sptr < Call>> queue;
		
		bool active = false;
		
		std::map<std::size_t, std::string> assigned;
		std::map<std::string, Bucket> buckets;
		
		Bucket *getNext(){
			Bucket *next = nullptr;
			std::time_t first = std::numeric_limits<std::time_t>::max();
			for(
				auto &bucket : buckets
				){
				if(bucket.second.remaining - bucket.second.transit.total() > 0 &&
				   first > bucket.second.queue.front()->created){
					next = &bucket.second;
					first = next->queue.front()->created;
				}
			}
			return next;
		}
		
		bool anyBucketLimited(){
			for(
				auto &bucket : buckets
				){
				if(bucket.second.remaining - bucket.second.transit.total() <= 0){
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