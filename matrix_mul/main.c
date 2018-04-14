#include <CL/cl.h>
#include <string.h>
#include <math.h>
#include "io.h"

#define DIE(...) ({ fprintf(stderr, __VA_ARGS__); exit(1); })
#define CHK_CL_ERR(err) ({ if ((err) != CL_SUCCESS) DIE("OpenCL invocation at %s:%i failed with error code %i\n", __FILE__, __LINE__, err); })
#define HANDLE_CL_ERR(stmt) ({ cl_int err = (stmt); CHK_CL_ERR(err); })

cl_device_id cl_device(const char* platform_name) {
    cl_uint platform_count;
    HANDLE_CL_ERR(clGetPlatformIDs(0, NULL, &platform_count));
    if (platform_count == 0) DIE("No OpenCL platforms found\n");

    cl_platform_id* platforms = (cl_platform_id*) malloc(platform_count * sizeof(cl_platform_id));
    HANDLE_CL_ERR(clGetPlatformIDs(platform_count, platforms, NULL));

    size_t platform_name_len = strlen(platform_name);
    char buffer[platform_name_len + 1];
    cl_platform_id platform = 0;
    for (cl_uint i = 0; i < platform_count; i++) {
        HANDLE_CL_ERR(clGetPlatformInfo(platforms[i], CL_PLATFORM_NAME, sizeof(buffer), buffer, NULL));
        if (strncmp(buffer, platform_name, platform_name_len) == 0) {
            platform = platforms[i];
            break;
        }
    }

    if (!platform) DIE("No suitable platforms found\n");

    cl_device_id device;
    HANDLE_CL_ERR(clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, NULL));
    return device;
}

cl_kernel cl_kernel_from_src(cl_context context, cl_device_id device, const char* src_file) {
    char* kernel_src = read_file(src_file);
    if (!kernel_src) DIE("Unable to load kernel source from %s\n", src_file);

    cl_int err;

    cl_program program = clCreateProgramWithSource(context, 1, (const char**) &kernel_src, NULL, &err); CHK_CL_ERR(err);
    if (clBuildProgram(program, 0, NULL, "-cl-std=CL1.2", NULL, NULL) != CL_SUCCESS) {
        char buffer[2048];
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, sizeof(buffer), buffer, NULL);

        fprintf(stderr, "Failed to build the kernel from source, refer to the build log below:\n%s", buffer);
        exit(1);
    }

    /* kernel_name is the source file name without .cl extension */
    char* kernel_name = strndup(src_file, strlen(src_file) - 3);
    cl_kernel kernel = clCreateKernel(program, kernel_name, &err); CHK_CL_ERR(err);
    return kernel;
}

void read_matrix(const char* file_name, float* matrix, size_t matrix_size) {
    FILE* matrix_file = fopen(file_name, "r");
    if (!matrix_file) DIE("Unable to open %s for reading", file_name);

    for (size_t i = 0; i < matrix_size; i++)
        if (fscanf(matrix_file, "%f", &matrix[i]) != 1) DIE("Unable to read a matrix from %s", file_name);

    fclose(matrix_file);
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        printf("Usage: ./matrix_mul platform m n p, where:"
               "\n    platform is the OpenCL platform used, e.g. \"Intel Gen OCL Driver\""
               "\n    m-by-n specifies the dimensions of matrix A"
               "\n    n-by-p specifies the dimensions of matrix B");
        return 0;
    }
    const char* platform = argv[1];
    unsigned int M = (unsigned int) strtoul(argv[2], NULL, 10);
    unsigned int N = (unsigned int) strtoul(argv[3], NULL, 10);
    unsigned int P = (unsigned int) strtoul(argv[4], NULL, 10);

    size_t matrix_a_size = M * N;
    size_t matrix_b_size = N * P;
    size_t matrix_c_size = M * P;

    float* matrices = (float*) malloc((matrix_a_size + matrix_b_size + matrix_c_size * 2) * sizeof(float));
    float* matrix_a = &matrices[0];
    float* matrix_b = &matrices[matrix_a_size];
    float* matrix_c_expected = &matrices[matrix_a_size + matrix_b_size];
    float* matrix_c_actual = &matrices[matrix_a_size + matrix_b_size + matrix_c_size];
    read_matrix("matrix_a", matrix_a, M * N);
    read_matrix("matrix_b", matrix_b, N * P);
    read_matrix("matrix_c", matrix_c_expected, M * P);

    cl_int err;

    cl_device_id device = cl_device(platform);
    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &err); CHK_CL_ERR(err);
    cl_command_queue commands = clCreateCommandQueueWithProperties(context, device, (cl_queue_properties[])
      { CL_QUEUE_PROPERTIES, CL_QUEUE_PROFILING_ENABLE, 0 }, &err); CHK_CL_ERR(err);
    cl_kernel kernel = cl_kernel_from_src(context, device, "simple.cl");

    cl_mem cl_matrix_a = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(float) * matrix_a_size, NULL, &err); CHK_CL_ERR(err);
    cl_mem cl_matrix_b = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(float) * matrix_b_size, NULL, &err); CHK_CL_ERR(err);
    cl_mem cl_matrix_c = clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(float) * matrix_c_size, NULL, &err); CHK_CL_ERR(err);

    HANDLE_CL_ERR(clEnqueueWriteBuffer(commands, cl_matrix_a, CL_TRUE, 0, sizeof(float) * matrix_a_size, matrix_a, 0, NULL, NULL));
    HANDLE_CL_ERR(clEnqueueWriteBuffer(commands, cl_matrix_b, CL_TRUE, 0, sizeof(float) * matrix_b_size, matrix_b, 0, NULL, NULL));
    HANDLE_CL_ERR(clSetKernelArg(kernel, 0, sizeof(cl_matrix_a), &cl_matrix_a));
    HANDLE_CL_ERR(clSetKernelArg(kernel, 1, sizeof(cl_matrix_b), &cl_matrix_b));
    HANDLE_CL_ERR(clSetKernelArg(kernel, 2, sizeof(cl_matrix_c), &cl_matrix_c));
    HANDLE_CL_ERR(clSetKernelArg(kernel, 3, sizeof(M), &M));
    HANDLE_CL_ERR(clSetKernelArg(kernel, 4, sizeof(N), &N));
    HANDLE_CL_ERR(clSetKernelArg(kernel, 5, sizeof(P), &P));

    size_t work_items;
    HANDLE_CL_ERR(clGetKernelWorkGroupInfo(kernel, device, CL_KERNEL_WORK_GROUP_SIZE, sizeof(work_items), &work_items, NULL));
    work_items = (size_t) sqrt(work_items); /* work group size is given for _all_ dimensions, we have two */

    size_t global_work_size[] = { M, P };
    size_t local_work_size[] = { work_items, work_items };
    printf("Global work size: %zu x %zu, local work size: %zu x %zu\n",
           global_work_size[0], global_work_size[1], local_work_size[0], local_work_size[1]);

    cl_event kernel_exec;

    HANDLE_CL_ERR(clEnqueueNDRangeKernel(commands, kernel, 2, NULL, global_work_size, local_work_size, 0, NULL, &kernel_exec));
    HANDLE_CL_ERR(clWaitForEvents(1, &kernel_exec));
    HANDLE_CL_ERR(clFinish(commands));
    HANDLE_CL_ERR(clEnqueueReadBuffer(commands, cl_matrix_c, CL_TRUE, 0, sizeof(float) * matrix_c_size, matrix_c_actual, 0, NULL, NULL));

    /* Validate results */
    for (size_t m = 0; m < M; m++) {
        for (size_t p = 0; p < M; p++) {
            float expected = matrix_c_expected[m * M + p];
            float actual = matrix_c_actual[m * M + p];
            /* TODO: implement a proper comparison
             * refer to https://randomascii.wordpress.com/2012/02/25/comparing-floating-point-numbers-2012-edition */
            if (fabsf(expected - actual) > 0.02) {
                printf("Row %zu, col %zu: expected result is %.8f, actual is %.8f\n", m, p, expected, actual);
            }
        }
    }

    /* Print profiling info */
    cl_ulong time_queued;
    cl_ulong time_start;
    cl_ulong time_end;

    HANDLE_CL_ERR(clGetEventProfilingInfo(kernel_exec, CL_PROFILING_COMMAND_QUEUED, sizeof(time_queued), &time_queued, NULL));
    HANDLE_CL_ERR(clGetEventProfilingInfo(kernel_exec, CL_PROFILING_COMMAND_START, sizeof(time_start), &time_start, NULL));
    HANDLE_CL_ERR(clGetEventProfilingInfo(kernel_exec, CL_PROFILING_COMMAND_END, sizeof(time_end), &time_end, NULL));

    printf("Kernel execution time is %f [ms]\n", ((double) time_end - time_start) / 1000000.0);
    printf("Time from enqueueing to execution is %f [ms]\n", ((double) time_start - time_queued) / 1000000.0);
}