# Friction High Level Design

This Document details the archtictural design princples of Friction. It is meant to give a high level overview of the components at play as well as the architectural boundaries between them. 

## System Overview

There are three main classifications for componenets in Friction- producers, consumers, and data pathways. The below sections outline what each of these is responsible for:

1. **Producers**: Producers collect and supply instrumentation data in a nonintrusive, low impact way. The collected data is then given to a lock free queue for processing by a consumer. Producers must all adhere to a base interface to allow for easy extension of metric types. 

2. **Data Pathways**: The data pathway utilized by Friction is a single lock free ring buffer that safely and efficiently passes data between producers and consumers. The lock free queue will hold items of type std::variant to adhere to the variation point of types established by the producer.

3. **Consumers**: Consumers ingest the metric data inside the lock free queue and display or process it for consumption by the user


## Data flow
The data pathway in Friction is very simple and uniform for each data type and will follow this flow:
1. Producers monitor for events and ingest relevant data 
2. Producers Push onto data pathway  
3. Data remains in lock free queue until a consumer is ready to process it
4. Consumer pulls the data off the queue and processes it

## Design constraints and Principles

1. Determinism- Data must be processed as real time as possible to give the user accurate analytics on their application
2. Low overhead - All components must be made to have as little performance impact of the profiled process as possible. Any overhead will lead to inaccuracies of the information presented to the user
3. No waiting- Locks are strictly forbidden as they violate the prior two design principles and introduce uneccesarery overhead 