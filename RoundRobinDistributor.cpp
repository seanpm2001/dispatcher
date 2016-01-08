#include "RoundRobinDistributor.h"
#include "dbg.h"


RoundRobinDistributor::RoundRobinDistributor(std::vector<Host> *hosts): AbstractDistributor(hosts) {
    read_counter.store(0);
    int thread_count = 10;
    for (int i = 1; i <= thread_count; ++i) {
        thread_pool.emplace_back(&RoundRobinDistributor::execute, this);
    }
};

RoundRobinDistributor::~RoundRobinDistributor() {};

void RoundRobinDistributor::execute() {
    std::unique_ptr<HttpResponse> response;
    Host *host;

    while (1) {
        std::unique_lock<std::mutex> lck(m_queue_mtx);
        //std::cout << "waiting" << std::endl;
        while (m_parsedRequests.empty()) m_queue_cv.wait(lck);
        //std::cout << "notified" << std::endl;
        RequestTuple *request_tuple = m_parsedRequests.front();
        m_parsedRequests.pop();
        lck.unlock();
        
        host = &(cluster_nodes->at(request_tuple->host));
        debug("Request send to host %s:%d", host->getUrl().c_str(), host->getPort());
        response = host->executeRequest(request_tuple->request);

        sendResponse(std::move(response), request_tuple->socket);
    }
}


void RoundRobinDistributor::distribute(struct HttpRequest *request, int sock) {
    unsigned int host_id, counter;
    std::unique_lock<std::mutex> lck(m_queue_mtx);

    debug("Dispatch query.");

    counter = read_counter.fetch_add(1);
    //avoid numeric overflow, reset read count after half of unsigned int range queries
    if (counter == m_boundary) {
        read_counter.fetch_sub(m_boundary);
    }
    host_id = counter % cluster_nodes->size();

    struct RequestTuple *request_tuple = new RequestTuple();
    request_tuple->request = request;
    request_tuple->host = host_id;
    request_tuple->socket = sock;

    m_parsedRequests.push(request_tuple);
    m_queue_cv.notify_one();
}


void RoundRobinDistributor::sendToAll(struct HttpRequest *request, int sock) {
    debug("Load table.");
    for (Host host : *cluster_nodes) {
        std::unique_ptr<HttpResponse> response = host.executeRequest(request);
    }
    close(sock);
    // TODO send response
}



void RoundRobinDistributor::sendToMaster(struct HttpRequest *request, int sock) {
    debug("Send request to master.");
    std::unique_lock<std::mutex> lck(m_queue_mtx);

    struct RequestTuple *request_tuple = new RequestTuple();
    request_tuple->request = request;
    request_tuple->host = 0;
    request_tuple->socket = sock;

    m_parsedRequests.push(request_tuple);
    m_queue_cv.notify_one();
}


void RoundRobinDistributor::sendResponse(std::unique_ptr<HttpResponse> response, int sock) {
    char *buf;
    int allocatedBytes;
    char http_response[] = "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: Keep-Alive\r\n\r\n%s";
    if (response) {
    	allocatedBytes = asprintf(&buf, http_response, response->getContentLength(), response->getContent());
    } else {
	    allocatedBytes = asprintf(&buf, http_response, 0, "");
    }
    if (allocatedBytes == -1) {
        log_err("An error occurred while creating response.");
        send(sock, error_response, strlen(error_response), 0);
        close(sock);
        return;
    }
    send(sock, buf, strlen(buf), 0);
    free(buf);
    close(sock);
    debug("Closed socket");
}
