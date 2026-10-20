/* Filtro bilateral exato, em tons de cinza. MPI divide linhas da imagem. */
#include <mpi.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message) {
    fprintf(stderr, "Erro: %s\n", message);
    MPI_Abort(MPI_COMM_WORLD, 1);
    exit(1);
}

static void *alloc(size_t bytes) {
    void *p = malloc(bytes ? bytes : 1);
    if (!p) fail("memoria insuficiente");
    return p;
}

/* Tokens do cabecalho PGM; aceita comentarios e separador CRLF. */
static void token(FILE *f, char *s, size_t capacity) {
    int c;
    do {
        c = fgetc(f);
        if (c == '#') while (c != '\n' && c != EOF) c = fgetc(f);
    } while (c != EOF && isspace((unsigned char)c));
    size_t n = 0;
    while (c != EOF && !isspace((unsigned char)c)) {
        if (n + 1 >= capacity) fail("token PGM muito longo");
        s[n++] = (char)c;
        c = fgetc(f);
    }
    if (!n) fail("cabecalho PGM incompleto");
    if (c == '\r') {
        int next = fgetc(f);
        if (next != '\n' && next != EOF) ungetc(next, f);
    }
    s[n] = '\0';
}

static int integer(const char *s, int maximum) {
    char *end;
    errno = 0;
    long value = strtol(s, &end, 10);
    if (errno || end == s || *end || value < 1 || value > maximum)
        fail("inteiro fora do intervalo permitido");
    return (int)value;
}

static double positive(const char *s) {
    char *end;
    errno = 0;
    double value = strtod(s, &end);
    if (errno || end == s || *end || !isfinite(value) || value < 0.01 || value > 10000)
        fail("sigma deve estar entre 0.01 e 10000");
    return value;
}

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (argc < 6) fail("uso: bilateral raio sigma_espacial sigma_intensidade pasta_saida entrada1.pgm [entrada2.pgm ...]");
    int radius = integer(argv[1], 128);
    double spatial_sigma = positive(argv[2]), range_sigma = positive(argv[3]);
    char host[MPI_MAX_PROCESSOR_NAME];
    int length;
    MPI_Get_processor_name(host, &length);
    fprintf(stderr, "rank=%d/%d host=%s\n", rank, size, host);

    long long total_pixels = 0;
    for (int file = 5; file < argc; ++file) {
        int dimensions[2] = {0, 0};
        unsigned char *image = NULL;
        if (rank == 0) {
            FILE *f = fopen(argv[file], "rb");
            if (!f) fail("nao foi possivel abrir a entrada");
            char s[64];
            token(f, s, sizeof(s));
            if (strcmp(s, "P5")) fail("use PGM binario P5 de 8 bits");
            token(f, s, sizeof(s)); dimensions[0] = integer(s, INT_MAX);
            token(f, s, sizeof(s)); dimensions[1] = integer(s, INT_MAX);
            token(f, s, sizeof(s));
            if (strcmp(s, "255")) fail("PGM deve ter maxval 255");
            if (dimensions[0] > INT_MAX / dimensions[1]) fail("imagem excede limite MPI de INT_MAX pixels");
            size_t pixels = (size_t)dimensions[0] * dimensions[1];
            image = alloc(pixels);
            if (fread(image, 1, pixels, f) != pixels) fail("imagem PGM truncada");
            fclose(f);
        }
        MPI_Bcast(dimensions, 2, MPI_INT, 0, MPI_COMM_WORLD);
        int width = dimensions[0], height = dimensions[1], pixels = width * height;
        total_pixels += pixels;
        if (rank != 0) image = alloc((size_t)pixels);
        /* Replicacao simples: cada rank tem tambem os pixels vizinhos das bordas. */
        MPI_Bcast(image, pixels, MPI_UNSIGNED_CHAR, 0, MPI_COMM_WORLD);
        int *counts = alloc((size_t)size * sizeof(int));
        int *offsets = alloc((size_t)size * sizeof(int));
        for (int p = 0; p < size; ++p) {
            int first = (int)((long long)height * p / size);
            int last = (int)((long long)height * (p + 1) / size);
            counts[p] = (last - first) * width;
            offsets[p] = first * width;
        }
        int first = offsets[rank] / width;
        int last = first + counts[rank] / width;
        unsigned char *local = alloc((size_t)counts[rank]);
        unsigned char *output = rank == 0 ? alloc((size_t)pixels) : NULL;
        int diameter = 2 * radius + 1;
        double *spatial = alloc((size_t)diameter * diameter * sizeof(double));
        for (int dy = -radius; dy <= radius; ++dy)
            for (int dx = -radius; dx <= radius; ++dx)
                spatial[(dy + radius) * diameter + dx + radius] =
                    exp(-(double)(dx * dx + dy * dy) / (2 * spatial_sigma * spatial_sigma));
        double inverse_range = 1.0 / (2 * range_sigma * range_sigma);

        for (int y = first; y < last; ++y) {
            for (int x = 0; x < width; ++x) {
                double sum = 0, weights = 0;
                int center = image[y * width + x];
                int y0 = y < radius ? -y : -radius;
                int y1 = height - 1 - y < radius ? height - 1 - y : radius;
                int x0 = x < radius ? -x : -radius;
                int x1 = width - 1 - x < radius ? width - 1 - x : radius;
                for (int dy = y0; dy <= y1; ++dy) {
                    for (int dx = x0; dx <= x1; ++dx) {
                        int neighbor = image[(y + dy) * width + x + dx];
                        double difference = neighbor - center;
                        double weight = spatial[(dy + radius) * diameter + dx + radius]
                                      * exp(-difference * difference * inverse_range);
                        sum += weight * neighbor;
                        weights += weight;
                    }
                }
                /* O pixel central tem peso 1; denominador nunca e zero. */
                local[(y - first) * width + x] = (unsigned char)(sum / weights + 0.5);
            }
        }
        MPI_Gatherv(local, counts[rank], MPI_UNSIGNED_CHAR, output, counts, offsets,
                    MPI_UNSIGNED_CHAR, 0, MPI_COMM_WORLD);
        if (rank == 0) {
            const char *name = strrchr(argv[file], '/');
            name = name ? name + 1 : argv[file];
            size_t path_size = strlen(argv[4]) + strlen(name) + 2;
            char *path = alloc(path_size);
            snprintf(path, path_size, "%s/%s", argv[4], name);
            FILE *f = fopen(path, "wb");
            free(path);
            if (!f) fail("nao foi possivel abrir a saida");
            if (fprintf(f, "P5\n%d %d\n255\n", width, height) < 0 ||
                fwrite(output, 1, (size_t)pixels, f) != (size_t)pixels)
                fail("falha ao escrever a imagem");
            if (fclose(f)) fail("falha ao fechar a imagem");
        }
        free(spatial); free(output); free(local); free(offsets); free(counts); free(image);
    }
    if (rank == 0)
        printf("Concluido: %d imagens, %lld pixels, %d processos MPI.\n",
               argc - 5, total_pixels, size);
    MPI_Finalize();
    return 0;
}
