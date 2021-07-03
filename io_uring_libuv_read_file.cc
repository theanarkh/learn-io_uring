#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <liburing.h>
#include <stdlib.h>
#include <uv.h>

#define QUEUE_DEPTH 1
#define BLOCK_SZ    1024

// 前向声明
struct reuqest;
// 定义回调
typedef void (*request_cb)(struct reuqest*);
struct file_context {
    int offset;
    int fd;
    int buf_size;
};
// 管理一个文件读取请求的结构体
struct reuqest {
    int fd;
    // 回调
    request_cb cb;
    // 字节大小
    int count;
    struct file_context* context;
    int nvecs;
    // 数据
    struct iovec iovecs[];     
};

// io_uring相关的结构体
struct io_uring_info {
  int fd;
  int32_t pending;
  struct io_uring ring;
  uv_poll_t poll_handle;
};



// io_uring完成任务后，Libuv执行的回调
void io_uring_done(uv_poll_t* handle, int status, int events) {
    
    struct io_uring* ring;
    struct io_uring_info* io_uring_data;
    struct io_uring_cqe* cqe;
    struct reuqest* req;
    // 获取Libuv中保存的io_uring信息
    io_uring_data = uv_default_loop()->data;
    ring = &io_uring_data->ring;
    // 处理每一个完成的请求
    while (1) { 
        io_uring_peek_cqe(ring, &cqe);

        if (cqe == NULL)
            break;
        // 全部处理完则注销事件
        if (--io_uring_data->pending == 0)
           uv_poll_stop(handle);
        // 拿到请求上下文
        req = (void*) (uintptr_t) cqe->user_data;
        // 记录读取的大小
        req->count = cqe->res;

        io_uring_cq_advance(ring, 1);
        // 执行回调
        req->cb(req);
    }
    // 处理完则退出
    if (io_uring_data->pending == 0)
        uv_stop(uv_default_loop());
}

// 向内核提交一个请求
int submit_read_request(int op, int fd, struct file_context* context, request_cb cb, struct io_uring *ring) {
    int current_block = 0;
    int count = context->buf_size;
    int blocks = (int) count / BLOCK_SZ;
    if (count % BLOCK_SZ) blocks++;
    // 申请内存
    struct reuqest *req = malloc(sizeof(*req) + (sizeof(struct iovec) * blocks));

    // 计算和申请保存文件内容的内存
    while (count) {
        // 剩下的大小
        off_t bytes_to_read = count;
        // 一个buffer最大保存BLOCK_SZ大小
        if (bytes_to_read > BLOCK_SZ)
            bytes_to_read = BLOCK_SZ;
        // 记录buffer大小
        req->iovecs[current_block].iov_len = bytes_to_read;
        // 申请内存
        void *buf;
        if( posix_memalign(&buf, BLOCK_SZ, BLOCK_SZ)) {
            perror("posix_memalign");
            return 1;
        }
        // 记录内存地址
        req->iovecs[current_block].iov_base = buf;
        // 下一块
        current_block++;
        // 更新剩下的大小
        count -= bytes_to_read;
    }
    // 获取一个io_uring的请求结构体
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    // 填充请求
    io_uring_prep_readv(sqe, fd, req->iovecs, blocks, context->offset);
    // 保存请求上下文，响应的时候用
    io_uring_sqe_set_data(sqe, req);
    req->fd = fd;
    // 保存回调
    req->cb = cb;
    req->nvecs = blocks;
    req->context = context;
    struct io_uring_info *io_uring_data = uv_default_loop()->data;
    io_uring_data->pending++;
    if (io_uring_data->pending == 1)
        // 注册事件和回调
        uv_poll_start(&io_uring_data->poll_handle, UV_READABLE, io_uring_done);
    // 提交请求给内核
    io_uring_submit(ring);

    return 0;
}

// 文件读取后的业务回调
void filedone(struct reuqest* req) {
    for (int i = 0; i < req->nvecs; i++) {
        printf("%s\n", (char *)req->iovecs[i].iov_base);
        free((void *)req->iovecs[i].iov_base);
    }
    
    if (req->count > 0) {
        struct file_context* context = req->context;
        struct io_uring_info *io_uring_data = uv_default_loop()->data;
        context->offset += req->count;
        submit_read_request(IORING_OP_READV, req->fd, context, filedone, &io_uring_data->ring);
    }
}

int main(int argc, char *argv[]) {
    
    if (argc < 2) {
        fprintf(stderr, "请输入文件名称\n");
        return 1;
    }
    // 申请一个io_uring相关的结构体
    struct io_uring_info *io_uring_data = malloc(sizeof(*io_uring_data));
    // 初始化io_uring
    io_uring_queue_init(1, &io_uring_data->ring, 0);
    // 初始化poll handle，保存监听的fd
    uv_poll_init(uv_default_loop(), &io_uring_data->poll_handle, io_uring_data->ring.ring_fd);
    
    // 保存io_uring的上下文在loop中
    uv_default_loop()->data = (void *)io_uring_data;
    // 处理每一个文件
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        struct file_context context;
        context.fd = 0;
        context.offset = 0;
        context.buf_size = 2048;
        submit_read_request(IORING_OP_READV, fd, &context, filedone, &io_uring_data->ring);
    }
    // 开始事件循环
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    // 退出
    uv_loop_close(uv_default_loop());
    io_uring_queue_exit(&io_uring_data->ring);
    return 0;
}
