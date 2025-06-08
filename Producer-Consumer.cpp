#include <iostream>
#include <thread>
#include <vector>
#include <queue>
#include <random>
#include <mutex>
#include <semaphore>
#include <chrono>
#include <atomic>
#include <csignal>
#include <stdexcept>
#include <future>

// Global shutdown flag to handle graceful shutdown requests 
std::atomic<bool> globalShutdown{false};

// Signal handler to handle Ctrl+C (SIGINT) for graceful termination 
void signalHandler(int) {
    std::cout << "\nShutdown requested...\n";
    globalShutdown = true;
}

// Circular Queue Class - Implements a thread-safe circular buffer
class CircularQueue {
public:
    CircularQueue(size_t bufferSize) : size(bufferSize), front(0), rear(0), count(0) {
        if (bufferSize == 0) {
            throw std::invalid_argument("Queue size must be greater than 0");
        }
        queue.resize(bufferSize); // Allocate space for the buffer
    }

    // Add a job to the queue - returns false if the queue is full 
    bool enqueue(int job) {
        if (count == size) return false;  // Queue is full
        queue[rear] = job; // Insert the job
        rear = (rear + 1) % size; // Advance rear index circularly 
        count++; // Increase job count
        return true;
    }

    // Remove a job from the queue - returns false if the queue is empty 
    bool dequeue(int& job) {
        if (count == 0) return false;  // Queue is empty
        job = queue[front];
        front = (front + 1) % size;
        count--;
        return true;
    }

    // Check if the queue is empty 
    bool isEmpty() const { return count == 0; }

private:
    std::vector<int> queue; // Storage for the circular buffer
    size_t size; // Maximum size of the queue 
    size_t front; // Index of the front element
    size_t rear; // Index of the rear element 
    size_t count; // Current number of elements in the queue 
};

// Producer Class - Handles job creation and insertion into the queue
class Producer {
public:
    Producer(CircularQueue& q, std::counting_semaphore<>& empty, std::counting_semaphore<>& full,
             std::mutex& mtx, std::atomic<int>& produced, int id)
        : queue(q), emptySlots(empty), fullSlots(full), queueMutex(mtx), totalJobsProduced(produced), producerID(id) {}

    // Produce jobs and inset them into the queue 
    void produce(int jobsPerProducer) {
        std::mt19937 gen(std::random_device{}()); // Random number generator 
        std::uniform_int_distribution<int> dis(1, 10); // Job durations between 1 and 10 seconds

        for (int i = 0; i < jobsPerProducer && !globalShutdown; ++i) {
            int jobTime = dis(gen); // Generate a random job duration  

            // Try to acquire an empty slot with a 10-second timeout
            if (!emptySlots.try_acquire_for(std::chrono::seconds(10))) {
                std::lock_guard<std::mutex> lock(queueMutex);
                std::cout << "Producer " << producerID << " timed out after 10 seconds.\n";
                break;
            }

            // Acquire the mutex and add the job to the queue 
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (queue.enqueue(jobTime)) {
                    ++totalJobsProduced; // Increment global produced job counter 
                    std::cout << "Producer " << producerID << " added job of duration: " << jobTime << " seconds\n";
                }
            }
            // Release a slot in the "full" semaphore to signal consumers 
            fullSlots.release();  
        }
    }

private:
    CircularQueue& queue; // Shared job queue 
    std::counting_semaphore<>& emptySlots; // Tracks available empty slots
    std::counting_semaphore<>& fullSlots; // Tracks available full slots
    std::mutex& queueMutex; // Mutex for queue synchronisation 
    std::atomic<int>& totalJobsProduced; // Global counter for jobs produced 
    int producerID; // ID of this producer 
};

// Consumer Class - Handles job consumption and processing
class Consumer {
public:
    Consumer(CircularQueue& q, std::counting_semaphore<>& empty, std::counting_semaphore<>& full,
             std::mutex& mtx, std::atomic<int>& consumed, int id)
        : queue(q), emptySlots(empty), fullSlots(full), queueMutex(mtx),
          totalJobsConsumed(consumed), consumerID(id), maxRetryCount(3) {}

    // Consumer jobs from the queue and process them 
    void consume() {
        while (!globalShutdown) {
            int retries = 0;

            // Try to acquire a job with a 10-second timeout
            while (!fullSlots.try_acquire_for(std::chrono::seconds(10))) {
                if (++retries >= maxRetryCount || globalShutdown) {
                    // Exit if max retries are reached or shutdown is requested 
                    if (queue.isEmpty()) {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        std::cout << "Consumer " << consumerID << " has no jobs left to process.\n";
                    }
                    return;
                }
            }

            int jobTime; // Variable to store the dequeued job 
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (queue.dequeue(jobTime)) {
                    ++totalJobsConsumed; // Increment global consumed job counter 
                    std::cout << "Consumer " << consumerID << " processing job of duration: "
                              << jobTime << " seconds\n";
                }
            }

            // Simulate job processing by sleeping for the job duration 
            std::this_thread::sleep_for(std::chrono::seconds(jobTime));  
            emptySlots.release();  // Release an empty slot for producers 
        }
    }

private:
    CircularQueue& queue; // Shared job queue 
    std::counting_semaphore<>& emptySlots; // Tracks available empty slots
    std::counting_semaphore<>& fullSlots; // Tracks available full slots
    std::mutex& queueMutex; // Mutex for queue synchronisation 
    std::atomic<int>& totalJobsConsumed; // Global counter for jobs consumed 
    int consumerID; // ID of this consumer 
    const int maxRetryCount; // Maximum retry attempts for acquiring a job
};

// Coordinator Class - Manages producer and consumer threads
class Coordinator {
public:
    Coordinator(int queueSize)
        : queue(queueSize), emptySlots(queueSize), fullSlots(0) {}

    // Start the producer and consumer threads
    void run(int jobsPerProducer, int numProducers, int numConsumers) {
        std::vector<std::thread> threads;

        // Create producer threads
        for (int i = 0; i < numProducers; ++i) {
            threads.emplace_back([this, jobsPerProducer, i]() {
                Producer producer(queue, emptySlots, fullSlots, queueMutex, totalJobsProduced, i + 1);
                producer.produce(jobsPerProducer);
            });
        }

        // Create consumer threads
        for (int i = 0; i < numConsumers; ++i) {
            threads.emplace_back([this, i]() {
                Consumer consumer(queue, emptySlots, fullSlots, queueMutex, totalJobsConsumed, i + 1);
                consumer.consume();
            });
        }

        // Wait for all threads to finish
        for (auto& thread : threads) {
            thread.join();
        }

        // Print summary
        printSummary(numProducers * jobsPerProducer);
    }

private:
    CircularQueue queue; // Shared job queue
    std::mutex queueMutex; // Mutex for queue synchronisation 
    std::counting_semaphore<> emptySlots; // Semaphore to track empty slots
    std::counting_semaphore<> fullSlots; // Semaphore to track full slots
    std::atomic<int> totalJobsProduced{0}; // Global counter for jobs produced 
    std::atomic<int> totalJobsConsumed{0}; // Global counter for jobs consumed

    // Print a summary of jobs produced and consumed 
    void printSummary(int expectedJobs) {
        std::cout << "\nSummary:\n";
        std::cout << "Total jobs produced: " << totalJobsProduced << "\n";
        std::cout << "Total jobs consumed: " << totalJobsConsumed << "\n";

        if (totalJobsProduced != expectedJobs || totalJobsConsumed != expectedJobs) {
            std::cerr << "ERROR: Job mismatch detected!\n";
            std::cerr << "Expected jobs: " << expectedJobs << ", but produced: " << totalJobsProduced << " and consumed: " << totalJobsConsumed << ".\n";
        } else {
            std::cout << "All jobs processed successfully.\n";
        }
    }
};

// Utility function to validate and parse command line arguments
int validateArg(const char* arg, const char* name) {
    try {
        int value = std::stoi(arg); // Convert argument to an integer 
        if (value <= 0) throw std::runtime_error(std::string(name) + " must be positive");
        return value;
    } catch (const std::invalid_argument&) {
        throw std::runtime_error(std::string(name) + " must be a valid number");
    }
}

int main(int argc, char* argv[]) {
    try {
        std::signal(SIGINT, signalHandler);

        if (argc != 5) {
            throw std::runtime_error("Usage: <queue_size> <jobs_per_producer> <num_producers> <num_consumers>");
        }

        int queueSize = validateArg(argv[1], "Queue size");
        int jobsPerProducer = validateArg(argv[2], "Jobs per producer");
        int numProducers = validateArg(argv[3], "Number of producers");
        int numConsumers = validateArg(argv[4], "Number of consumers");

        Coordinator coordinator(queueSize);
        coordinator.run(jobsPerProducer, numProducers, numConsumers);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}