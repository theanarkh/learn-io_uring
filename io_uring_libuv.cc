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
struct file_info;
// 定义回调
typedef void (*file_callback)(struct file_info*);
// 管理一个文件读取请求的结构体
struct file_info {
    // 文件大小
    off_t file_sz;
    // 回调
    file_callback cb;
    // 读取的大小
    int count;
    // 文件名
    char *name;
    // 读取的数据
    struct iovec iovecs[];     
};

// 获取文件大小
off_t get_file_size(int fd) {
    struct stat st;

    if(fstat(fd, &st) < 0) {
        perror("fstat");
        return -1;
    }
    if (S_ISBLK(st.st_mode)) {
        unsigned long long bytes;
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
            perror("ioctl");
            return -1;
        }
        return bytes;
    } else if (S_ISREG(st.st_mode))
        return st.st_size;

    return -1;
}

// 向内核提交一个请求
int submit_read_request(char *file_path, file_callback cb, struct io_uring *ring) {
    // 打开文件
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        perror("open");
        return 1;
    }
    // 获取大小
    off_t file_sz = get_file_size(file_fd);
    off_t bytes_remaining = file_sz;
    int current_block = 0;
    int blocks = (int) file_sz / BLOCK_SZ;
    if (file_sz % BLOCK_SZ) blocks++;
    // 申请内存
    struct file_info *fi = malloc(sizeof(*fi) + (sizeof(struct iovec) * blocks));
    // 保存文件名
    fi->name = file_path;
    // 计算和申请保存文件内容的内存
    while (bytes_remaining) {
        // 剩下的大小
        off_t bytes_to_read = bytes_remaining;
        // 一个buffer最大保存BLOCK_SZ大小
        if (bytes_to_read > BLOCK_SZ)
            bytes_to_read = BLOCK_SZ;
        // 记录buffer大小
        fi->iovecs[current_block].iov_len = bytes_to_read;
        // 申请内存
        void *buf;
        if( posix_memalign(&buf, BLOCK_SZ, BLOCK_SZ)) {
            perror("posix_memalign");
            return 1;
        }
        // 记录内存地址
        fi->iovecs[current_block].iov_base = buf;
        // 下一块
        current_block++;
        // 更新剩下的大小
        bytes_remaining -= bytes_to_read;
    }
    // 保存文件大小
    fi->file_sz = file_sz;
    // 获取一个io_uring的请求结构体
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    // 填充请求
    io_uring_prep_readv(sqe, file_fd, fi->iovecs, blocks, 0);
    // 保存请求上下文，响应的时候用
    io_uring_sqe_set_data(sqe, fi);
    // 保存回调
    fi->cb = cb;
    // 提交请求给内核
    io_uring_submit(ring);

    return 0;
}

// io_uring相关的结构体
struct io_uring_info {
  int fd;
  int32_t pending;
  struct io_uring ring;
  uv_poll_t poll_handle;
};

// io_uring完成任务后，Libuv执行的回调
void uv__io_uring_done(uv_poll_t* handle, int status, int events) {
    
    struct io_uring* ring;
    struct io_uring_info* io_uring_data;
    struct io_uring_cqe* cqe;
    struct file_info* req;
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

// 文件读取后的业务回调
void filedone(struct file_info* info) {
    printf("读取的大小：%d，文件信息：%s => %d\n", (int)info->count, info->name, (int)info->file_sz);
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
    // 注册事件和回调
    uv_poll_start(&io_uring_data->poll_handle, UV_READABLE, uv__io_uring_done);
    // 保存io_uring的上下文在loop中
    uv_default_loop()->data = (void *)io_uring_data;
    // 处理每一个文件
    for (int i = 1; i < argc; i++) {
        submit_read_request(argv[i], filedone, &io_uring_data->ring);
        io_uring_data->pending++;
    }
    // 开始事件循环
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    // 退出
    uv_loop_close(uv_default_loop());
    io_uring_queue_exit(&io_uring_data->ring);
    return 0;
}