
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#define MAX_SIZE 1024
#define MAX_CHILDREN 10
#define MAX_DATA_SIZE 256

typedef enum {
    FILE_MF = 0x79,  // 79 -> MF
    FILE_DF = 0x78,  // 78 -> DF
    FILE_EF = 0x41   // 0x41 -> Transparent EF
} FileType;

typedef struct File {
    FileType type;
    uint16_t id;     // Changed from fid to id to match variable usage in parse_apdu
    int size;
    char* data;      // To store data for EF, DF, and MF
    struct File* parent;
    struct File* children[MAX_CHILDREN];
    int childCount;
} File;

File* MF = NULL;
File* current_dir = NULL;
int total_size = MAX_SIZE;
int used_size = 0;

// Function prototypes
File* find_by_fid(File* file, uint16_t fid);
File* select_file(uint16_t fid);
File* create(FileType type, uint16_t fid, int size, File* parent, const char* data);
//void return_status(uint16_t status);
void return_data_and_status(const char* data, size_t data_len, uint16_t status);
void generate_fcp_data(File* file, char* buffer, size_t buffer_size);

const char* get_type_str(FileType type) {
    return type == FILE_MF ? "MF" : type == FILE_DF ? "DF" : "EF";
}

FileType parse_file_type(uint8_t tag) {
    if (tag == 0x79) return FILE_MF;
    if (tag == 0x78) return FILE_DF;
    if (tag == 0x41) return FILE_EF;
    return FILE_EF; // Default case
}

void to_upper(char* str) {
    for (int i = 0; str[i]; ++i)
        str[i] = toupper((unsigned char)str[i]);
}

// Function to load file structure from the binary file
int load_file_structure() {
    FILE* fp = NULL;
    errno_t err = fopen_s(&fp, "memory_store.bin", "rb");
    if (err != 0 || fp == NULL) {
        printf("No existing memory_store.bin found. Starting fresh.\n");
        return 0;
    }

    // First determine file size to ensure we don't read beyond the file
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        printf("Empty memory_store.bin file. Starting fresh.\n");
        fclose(fp);
        return 0;
    }

    // Read MF data
    uint8_t high, low;
    uint8_t type_byte;
    int df_count;

    // Read MF FID (2 bytes) and type (1 byte)
    if (fread(&high, sizeof(uint8_t), 1, fp) != 1 ||
        fread(&low, sizeof(uint8_t), 1, fp) != 1 ||
        fread(&type_byte, sizeof(uint8_t), 1, fp) != 1) {
        printf("Failed to read MF metadata. Starting fresh.\n");
        fclose(fp);
        return 0;
    }

    uint16_t mf_fid = (high << 8) | low;
    FileType mf_type = (FileType)type_byte;

    // Read DF count in MF
    if (fread(&df_count, sizeof(int), 1, fp) != 1) {
        printf("Failed to read DF count. Starting fresh.\n");
        fclose(fp);
        return 0;
    }

    // Skip DF FIDs array as we'll reconstruct it from actual DFs
    fseek(fp, MAX_CHILDREN * sizeof(uint16_t), SEEK_CUR);

    // Read MF data size and content
    int mf_size;
    if (fread(&mf_size, sizeof(int), 1, fp) != 1) {
        printf("Failed to read MF size. Starting fresh.\n");
        fclose(fp);
        return 0;
    }

    char* mf_data = (char*)malloc(mf_size + 1);
    if (!mf_data) {
        printf("Memory allocation failed for MF data. Starting fresh.\n");
        fclose(fp);
        return 0;
    }

    if (fread(mf_data, sizeof(char), mf_size, fp) != mf_size) {
        printf("Failed to read MF data content. Starting fresh.\n");
        free(mf_data);
        fclose(fp);
        return 0;
    }
    mf_data[mf_size] = '\0'; // Null-terminate the string

    // Create MF in memory
    MF = (File*)malloc(sizeof(File));
    if (!MF) {
        printf("Memory allocation failed for MF. Starting fresh.\n");
        free(mf_data);
        fclose(fp);
        return 0;
    }

    MF->type = mf_type;
    MF->id = mf_fid;
    MF->size = mf_size;
    MF->data = mf_data;
    MF->parent = NULL;
    MF->childCount = 0;
    memset(MF->children, 0, sizeof(MF->children));

    current_dir = MF;
    used_size = mf_size;

    printf("Loaded MF: FID=0x%04X, Size=%d\n", MF->id, MF->size);

    // Read DF entries
    for (int i = 0; i < df_count; i++) {
        // Check if we're at the end of the file
        if (ftell(fp) >= file_size) break;

        // Read DF FID and type
        if (fread(&high, sizeof(uint8_t), 1, fp) != 1 ||
            fread(&low, sizeof(uint8_t), 1, fp) != 1 ||
            fread(&type_byte, sizeof(uint8_t), 1, fp) != 1) {
            printf("Error reading DF metadata. Stopping load.\n");
            break;
        }

        uint16_t df_fid = (high << 8) | low;
        FileType df_type = (FileType)type_byte;

        // Read DF size
        int df_size;
        if (fread(&df_size, sizeof(int), 1, fp) != 1) {
            printf("Error reading DF size. Stopping load.\n");
            break;
        }

        // Read DF data
        char* df_data = (char*)malloc(df_size + 1);
        if (!df_data) {
            printf("Memory allocation failed for DF data. Stopping load.\n");
            break;
        }

        if (fread(df_data, sizeof(char), df_size, fp) != df_size) {
            printf("Error reading DF data. Stopping load.\n");
            free(df_data);
            break;
        }
        df_data[df_size] = '\0'; // Null-terminate

        // Create DF in memory
        File* df = (File*)malloc(sizeof(File));
        if (!df) {
            printf("Memory allocation failed for DF. Stopping load.\n");
            free(df_data);
            break;
        }

        df->type = df_type;
        df->id = df_fid;
        df->size = df_size;
        df->data = df_data;
        df->parent = MF;
        df->childCount = 0;
        memset(df->children, 0, sizeof(df->children));

        // Add DF to MF's children
        if (MF->childCount < MAX_CHILDREN) {
            MF->children[MF->childCount++] = df;
            used_size += df_size;
            printf("Loaded DF: FID=0x%04X, Size=%d\n", df->id, df->size);
        }
        else {
            printf("MF has too many children, can't add DF: 0x%04X\n", df_fid);
            free(df->data);
            free(df);
            break;
        }
    }

    // Read EF entries - they follow the pattern: 'E' + FID(2) + size(4) + type(1) + data
    while (ftell(fp) < file_size) {
        char ef_tag;
        if (fread(&ef_tag, sizeof(char), 1, fp) != 1 || ef_tag != 'E') {
            // Not an EF marker, skip
            fseek(fp, 1, SEEK_CUR);
            continue;
        }

        // Read EF FID, size, and type
        if (fread(&high, sizeof(uint8_t), 1, fp) != 1 ||
            fread(&low, sizeof(uint8_t), 1, fp) != 1) {
            printf("Error reading EF FID. Stopping EF load.\n");
            break;
        }

        uint16_t ef_fid = (high << 8) | low;

        int ef_size;
        if (fread(&ef_size, sizeof(int), 1, fp) != 1) {
            printf("Error reading EF size. Stopping EF load.\n");
            break;
        }

        if (fread(&type_byte, sizeof(uint8_t), 1, fp) != 1) {
            printf("Error reading EF type. Stopping EF load.\n");
            break;
        }
        FileType ef_type = (FileType)type_byte;

        // Find the parent DF for this EF
        // For simplicity, we'll find it by scanning through the first few bytes of the EF data
        // This is a simplified approach - in a real system, you'd have explicit parent-child links
        char* ef_data = (char*)malloc(ef_size + 1);
        if (!ef_data) {
            printf("Memory allocation failed for EF data. Stopping EF load.\n");
            break;
        }

        if (fread(ef_data, sizeof(char), ef_size, fp) != ef_size) {
            printf("Error reading EF data. Stopping EF load.\n");
            free(ef_data);
            break;
        }
        ef_data[ef_size] = '\0'; // Null-terminate

        // Find parent DF by comparing the first DF we find
        // In a real system, you would store parent-child relationships more explicitly
        File* parent_df = NULL;
        for (int i = 0; i < MF->childCount; i++) {
            if (MF->children[i]->type == FILE_DF) {
                parent_df = MF->children[i];
                break;
            }
        }

        if (parent_df == NULL) {
            printf("No parent DF found for EF 0x%04X. Skipping.\n", ef_fid);
            free(ef_data);
            continue;
        }

        // Create EF in memory
        File* ef = (File*)malloc(sizeof(File));
        if (!ef) {
            printf("Memory allocation failed for EF. Stopping load.\n");
            free(ef_data);
            break;
        }

        ef->type = ef_type;
        ef->id = ef_fid;
        ef->size = ef_size;
        ef->data = ef_data;
        ef->parent = parent_df;
        ef->childCount = 0; // EFs don't have children

        // Add EF to parent DF's children
        if (parent_df->childCount < MAX_CHILDREN) {
            parent_df->children[parent_df->childCount++] = ef;
            used_size += ef_size;
            printf("Loaded EF: FID=0x%04X, Size=%d, Parent=0x%04X\n",
                ef->id, ef->size, parent_df->id);
        }
        else {
            printf("Parent DF has too many children, can't add EF: 0x%04X\n", ef_fid);
            free(ef->data);
            free(ef);
        }
    }

    fclose(fp);
    return 1;
}

// Helper to write EF metadata into memory_store.bin
void write_ef_to_file(File* ef) {
    if (!ef || ef->type != FILE_EF || !ef->parent || ef->parent->type != FILE_DF) return;

    FILE* fp = NULL;
    errno_t err = fopen_s(&fp, "memory_store.bin", "rb+");
    if (err != 0 || fp == NULL) {
        printf("Failed to open memory_store.bin to write EF\n");
        return;
    }

    // Seek to the end of file for EF append
    fseek(fp, 0, SEEK_END);

    // EF layout: FID (2), Size (4), Type (1), Data (variable)
    fwrite(&ef->id, sizeof(uint16_t), 1, fp);
    fwrite(&ef->size, sizeof(int), 1, fp);

    uint8_t type_byte = (uint8_t)ef->type;
    fwrite(&type_byte, sizeof(uint8_t), 1, fp);

    fwrite(ef->data, sizeof(char), ef->size, fp);

    fclose(fp);
}

// Updated create function
File* create(FileType type, uint16_t fid, int size, File* parent, const char* data) {
    if (type == FILE_MF) {
        if (fid != 0x3F00) {
            printf("Invalid MF FID. MF must have FID=0x3F00.\n");
            return NULL;
        }

        // Check if MF already exists on disk
        FILE* check_fp = fopen("memory_store.bin", "rb");
        if (check_fp != NULL) {
            printf("MF already exists in file.\n");
            fclose(check_fp);
            return NULL;
        }

        // Create new MF file
        FILE* fp = fopen("memory_store.bin", "wb");
        if (fp == NULL) {
            printf("Failed to create memory_store.bin\n");
            return NULL;
        }

        // Write MF metadata
        uint8_t high = (fid >> 8) & 0xFF;
        uint8_t low = fid & 0xFF;
        fwrite(&high, sizeof(uint8_t), 1, fp);
        fwrite(&low, sizeof(uint8_t), 1, fp);

        uint8_t type_byte = (uint8_t)type;
        fwrite(&type_byte, sizeof(uint8_t), 1, fp);

        int initial_df_count = 0;
        fwrite(&initial_df_count, sizeof(int), 1, fp);

        // Reserve space for MAX_CHILDREN DF FIDs
        uint16_t empty_fid = 0x0000;
        for (int i = 0; i < MAX_CHILDREN; i++) {
            fwrite(&empty_fid, sizeof(uint16_t), 1, fp);
        }

        // Write MF size and data
        fwrite(&size, sizeof(int), 1, fp);
        if (data) {
            fwrite(data, sizeof(char), size, fp);
        }
        else {
            char* zeros = (char*)calloc(size, sizeof(char));
            fwrite(zeros, sizeof(char), size, fp);
            free(zeros);
        }

        fclose(fp);
    }

    // Ensure valid size and space for new file includes meta data size or size
    if (size < 0 || size > total_size) {
        printf("Invalid size: %d (max: %d)\n", size, total_size);
        return NULL;
    }

    if (used_size + size > total_size) {
        printf("Not enough space. Used: %d, Requested: %d, Max: %d\n", used_size, size, total_size);
        return NULL;
    }

    // Structural validation
    if (type == FILE_EF && (!parent || parent->type != FILE_DF)) {
        printf("EF must be inside a DF.\n");
        return NULL;
    }

    if (type == FILE_DF && (!parent || parent->type != FILE_MF)) {
        printf("DF must be inside MF.\n");
        return NULL;
    }

    // Allocate memory for the file
    File* new_file = (File*)malloc(sizeof(File));
    if (!new_file) {
        printf("Memory allocation failed for new file.\n");
        return NULL;
    }

    // Initialize file structure
    new_file->type = type;
    new_file->id = fid;
    new_file->size = size;
    new_file->parent = parent;
    new_file->childCount = 0;

    new_file->data = (char*)malloc(size + 1);
    if (!new_file->data) {
        printf("Memory allocation failed for file data.\n");
        free(new_file);
        return NULL;
    }

    if (data) {
        strncpy_s(new_file->data, size + 1, data, _TRUNCATE);
    }
    else {
        memset(new_file->data, 0, size + 1);
    }

    // Attach file in hierarchy
    if (type == FILE_MF) {
        MF = new_file;
        current_dir = MF;
    }
    else if (parent->childCount < MAX_CHILDREN) {
        parent->children[parent->childCount++] = new_file;

        // --- DF Storage in BIN ---
        if (type == FILE_DF && parent == MF) {
            FILE* fp = NULL;
            errno_t err = fopen_s(&fp, "memory_store.bin", "rb+");
            if (err == 0 && fp != NULL) {
                // Update DF count in MF section
                fseek(fp, 3, SEEK_SET); // Skip MF FID (2) and type (1)
                int df_count;
                fread(&df_count, sizeof(int), 1, fp);
                df_count++;
                fseek(fp, 3, SEEK_SET);
                fwrite(&df_count, sizeof(int), 1, fp);

                // Update DF FID array
                fseek(fp, 3 + sizeof(int) + (df_count - 1) * sizeof(uint16_t), SEEK_SET);
                fwrite(&fid, sizeof(uint16_t), 1, fp);

                // Append DF at the end
                fseek(fp, 0, SEEK_END);

                // Write DF metadata
                uint8_t high = (fid >> 8) & 0xFF;
                uint8_t low = fid & 0xFF;
                uint8_t df_type = (uint8_t)type;

                fwrite(&high, sizeof(uint8_t), 1, fp);
                fwrite(&low, sizeof(uint8_t), 1, fp);
                fwrite(&df_type, sizeof(uint8_t), 1, fp);

                // Write DF size
                fwrite(&size, sizeof(int), 1, fp);

                // Write DF data
                if (data) {
                    fwrite(data, sizeof(char), size, fp);
                }
                else {
                    char* zeros = (char*)calloc(size, sizeof(char));
                    fwrite(zeros, sizeof(char), size, fp);
                    free(zeros);
                }

                fclose(fp);
            }
        }

        // --- EF Storage in BIN ---
        if (type == FILE_EF && parent->type == FILE_DF) {
            FILE* fp = NULL;
            errno_t err = fopen_s(&fp, "memory_store.bin", "rb+");
            if (err == 0 && fp != NULL) {
                fseek(fp, 0, SEEK_END);

                // EF marker
                char ef_tag = 'E';
                fwrite(&ef_tag, sizeof(char), 1, fp);

                // Write EF FID
                uint8_t high = (fid >> 8) & 0xFF;
                uint8_t low = fid & 0xFF;
                fwrite(&high, sizeof(uint8_t), 1, fp);
                fwrite(&low, sizeof(uint8_t), 1, fp);

                // Write EF size
                fwrite(&size, sizeof(int), 1, fp);

                // Write EF type
                uint8_t type_byte = (uint8_t)type;
                fwrite(&type_byte, sizeof(uint8_t), 1, fp);

                // Write parent DF FID for reference when loading
                uint16_t parent_fid = parent->id;
                fwrite(&parent_fid, sizeof(uint16_t), 1, fp);

                // Write EF data
                if (new_file->data) {
                    fwrite(new_file->data, sizeof(char), size, fp);
                }
                else {
                    char* empty = (char*)calloc(size, sizeof(char));
                    fwrite(empty, sizeof(char), size, fp);
                    free(empty);
                }

                fclose(fp);
            }
        }
    }
    else {
        printf("Parent has too many children (max: %d).\n", MAX_CHILDREN);
        free(new_file->data);
        free(new_file);
        return NULL;
    }
    size += sizeof(uint16_t) + sizeof(int) + sizeof(uint8_t); // FID, size, type
    if (type == FILE_EF) {
        size += sizeof(uint16_t); // Parent FID
    }
    used_size += size;

    printf("%s created: FID=0x%04X, Size=%d, Under=0x%04X\n",
        get_type_str(type), fid, size, parent ? parent->id : 0);

    return new_file;
} 

// ---------- Find by FID ----------
File* find_by_fid(File* file, uint16_t fid) {
    if (!file) return NULL;
    if (file->id == fid) return file;
    for (int i = 0; i < file->childCount; ++i) {
        File* result = find_by_fid(file->children[i], fid);
        if (result) return result;
    }
    return NULL;
}

// ---------- Select File (Modified to return File*) ----------
File* select_file(uint16_t fid) {
    File* file = find_by_fid(MF, fid);
    if (file) {
        printf("SELECTED: %s FID=0x%04X, Size=%d\n",
            get_type_str(file->type), file->id, file->size);

        if (file->type == FILE_DF) {
            current_dir = file;
        }
        else if (file->type == FILE_EF) {
            // If it's an EF, show the data inside it.
            if (file->data) {
                printf("Data inside EF (FID=0x%04X): %s\n", file->id, file->data);
            }
            else {
                printf("EF (FID=0x%04X) is empty.\n", file->id);
            }
        }
        else if (file->type == FILE_MF) {
            current_dir = file;
            if (file->data) {
                printf("Data inside MF (FID=0x%04X): %s\n", file->id, file->data);
            }
            else {
                printf("MF (FID=0x%04X) is empty.\n", file->id);
            }
        }
    }
    else {
        printf("File 0x%04X not found.\n", fid);
    }

    return file;  // Return the file pointer
}

// ---------- Helper function to return status words ----------
static void return_status(uint16_t status) {
    printf("Return status: 0x%04X\n", status);

    uint8_t sw1 = (status >> 8) & 0xFF;
    uint8_t sw2 = status & 0xFF;

    printf("SW1: 0x%02X, SW2: 0x%02X\n", sw1, sw2);


}


// ---------- Helper function to return data and status ----------
void return_data_and_status(const char* data, size_t data_len, uint16_t status) {
    printf("Return data [%zu bytes] and status: 0x%04X\n", data_len, status);

    // Extract SW1 and SW2 from the 16-bit status
    uint8_t sw1 = (status >> 8) & 0xFF;
    uint8_t sw2 = status & 0xFF;

}

// ---------- Generate FCP data for a file ----------
void generate_fcp_data(File* file, char* buffer, size_t buffer_size) {
    if (!file || !buffer || buffer_size == 0) return;

    // Simple implementation - encoding FCP template with file ID and size
    int length = 8; // Basic length for file ID and size tags

    snprintf(buffer, buffer_size,
        "62%02X"    // FCP template tag and length
        "83020%04X" // File ID tag, length (2), and value
        "8102%04X", // File size tag, length (2), and value
        length,
        file->id,
        file->size);

    // In a real implementation, you would include all relevant file attributes
}

// ---------- Memory ----------
static void memory_used() {
    printf("Memory Used: %d / %d bytes\n", used_size, total_size);
}

// ---------- Print ----------
static void print_tree(File* file, int level) {
    if (!file) return;
    for (int i = 0; i < level; ++i) printf("  ");
    printf("%s: FID=0x%04X, Size=%d", get_type_str(file->type), file->id, file->size);

    // Print data content for visibility
    if (file->data && strlen(file->data) > 0) {
        printf(", Data: %s", file->data);
    }

    printf("\n");

    for (int i = 0; i < file->childCount; ++i)
        print_tree(file->children[i], level + 1);
}

// ---------- APDU Parser (Updated to match the uploaded code) ----------
void parse_apdu(const char* apdu) {
    unsigned int cla, ins, p1, p2, lc;
    uint16_t fid = 0;
    int size = 0;
    FileType type = FILE_EF; // Default to avoid uninitialized use
    char data[MAX_DATA_SIZE] = { 0 };

    // Convert to uppercase input safely
    char input[256] = { 0 };
    if (strncpy_s(input, sizeof(input), apdu, _TRUNCATE) != 0) {
        printf("APDU copy failed.\n");
        return_status(0x6F00); // Technical problem
        return;
    }
    to_upper(input);

    //validate the values of cla , ins , p1 , p2
    if (sscanf_s(input, "%02X%02X%02X%02X", &cla, &ins, &p1, &p2) < 4) {
        printf("Invalid APDU format.\n");
        return_status(0x6700); // Wrong length
        return;
    }


    printf("CLA: 0x%02X, INS: 0x%02X, P1: 0x%02X, P2: 0x%02X", cla, ins, p1, p2);

    if (ins == 0xE0) {  // CREATE
        // Verify this is a standard CREATE command (CLA=0x00, P1=0x00, P2=0x00)
        if (cla != 0x00 || p1 != 0x00 || p2 != 0x00) {
            printf("Invalid parameters for CREATE. Expected CLA=0x00, P1=0x00, P2=0x00.\n");
            return_status(0x6A86); // Incorrect P1-P2
            return;
        }

        const char* ptr = input + 8; // Move past CLA, INS, P1, P2 (8 hex chars

        if (strlen(ptr) < 2 || strncmp(ptr, "62", 2) != 0) {
            printf("Missing FCP template tag (0x62). Invalid CREATE command data.\n");
            return_status(0x6A80); // Incorrect parameters in data field
            return;
        }

        lc = (unsigned int)(strlen(ptr) / 2);  // Each byte = 2 hex digits
        printf("\nCalculated Lc (data length): 0x%02X (%u bytes)\n", lc, lc);


        // Check for FCP template tag (0x62)
        unsigned int template_tag = 0, template_len = 0;
        if (strlen(ptr) >= 4) {
            if (sscanf_s(ptr, "%02X%02X", &template_tag, &template_len) == 2 && template_tag == 0x62) {
                printf("FCP Template Tag: 0x%02X, Length: %u\n", template_tag, template_len);
                ptr += 4; // Move past template tag and length
            }
            else {
                printf("Missing FCP template tag (0x62). Invalid CREATE command data.\n");
                return_status(0x6A80); // Incorrect parameters in data field
                return;
            }
        }

        while (*ptr && strlen(ptr) >= 4) {
            unsigned int tag = 0, len = 0;
            if (sscanf_s(ptr, "%02X%02X", &tag, &len) != 2) {
                printf("Tag/Len parse failed.\n");
                return_status(0x6A80); // Incorrect parameters in data field
                break;
            }
            ptr += 4;

            // Bounds check for len * 2 (hex string)
            if (len * 2 >= sizeof(data) || strlen(ptr) < len * 2) {
                printf("Value length too long or malformed (tag: 0x%02X, len: %u)\n", tag, len);
                return_status(0x6A80); // Incorrect parameters in data field
                break;
            }

            char val[256] = { 0 };
            strncpy_s(val, sizeof(val), ptr, len * 2);
            val[len * 2] = '\0';  // Null-terminate
            ptr += len * 2;

            printf("Tag: 0x%02X, Length: %u, Value: %s\n", tag, len, val);

            // Tag handling
            switch (tag) {
            case 0x82:  // File descriptor
            {
                unsigned int temp_type = 0;
                if (sscanf_s(val, "%02X", &temp_type) == 1) {
                    switch (temp_type) {
                    case 0x79: type = FILE_MF; break;
                    case 0x78: type = FILE_DF; break;
                    case 0x41: type = FILE_EF; break;
                    default:
                        printf("Unknown file type: 0x%02X\n", temp_type);
                        break;
                    }
                    printf("Parsed File Type: %d\n", type);
                }
            }
            break;
            case 0x83:  // File ID
                if (sscanf_s(val, "%04hX", &fid) == 1)
                    printf("File ID: 0x%04X\n", fid);
                break;
            case 0x8A:  // Life Cycle Status
                printf("Life Cycle Status: %s\n", val);
                break;
            case 0x8C:  // Security Attributes
                printf("Security Attributes: %s\n", val);
                break;
            case 0x80: {
                unsigned int temp_size = 0;
                if (sscanf_s(val, "%04X", &temp_size) != 1) {
                    to_upper(val);
                    printf("Failed to parse file size.\n");
                }
                else {
                    size = (int)temp_size;
                    printf("Parsed total file size: 0x%X\n", size);
                }
                break;
            }
            default:
                printf("Unknown tag in FCP: 0x%02X\n", tag);
                break;
            }
        }

        // Validate and create file
        if (size <= 0) {
            printf("Invalid file size: %d\n", size);
            return_status(0x6A80); // Incorrect parameters in data field
            return;
        }

        File* parent = NULL;
        if (type == FILE_DF) parent = MF;
        else if (type == FILE_EF) parent = current_dir;

        File* new_file = create(type, fid, size, parent, data);
        if (!new_file) {
            printf("File creation failed.\n");
            return_status(0x6A84); // Not enough memory space
            return;
        }
        return_status(0x9000); // Success
    }
    else if (ins == 0xA4) {  // SELECT
        if (p1 != 0x00 || p2 != 0x04) {
            printf("Invalid P1 or P2 for SELECT. Expected P1=0x00 and P2=0x04.\n");
            return_status(0x6A86); // Incorrect P1-P2
            return;
        }
        else if (p1 != 0x00 || p2 != 0x0C) {
            printf("Select the file , no FCP return");
            return_status(0x9000);
        }

        if (strlen(input) >= 14) {
            if (sscanf_s(input + 10, "%04hX", &fid) == 1)
                select_file(fid);
            else {
                printf("Failed to parse SELECT FID.\n");
                return_status(0x6A80); // Incorrect parameters in data field
            }
        }
        else {
            printf("SELECT missing FID.\n");
            return_status(0x6700); // Wrong length
        }
    }
    else {
        printf("Unsupported INS: 0x%02X\n", ins);
        return_status(0x6D00); // Instruction not supported
    }
}


// Write data to an existing EF
void write_data_to_ef(File* ef, const char* data, int offset, int length) {
    if (!ef || ef->type != FILE_EF) {
        printf("Can only write data to EF files.\n");
        return;
    }

    if (offset < 0 || offset + length > ef->size) {
        printf("Write operation out of bounds.\n");
        return;
    }

    // Update in-memory representation
    strncpy_s(ef->data + offset, ef->size - offset + 1, data, length);

    // Update file on disk
    FILE* fp = NULL;
    errno_t err = fopen_s(&fp, "memory_store.bin", "rb+");
    if (err != 0 || fp == NULL) {
        printf("Failed to open memory_store.bin for data update\n");
        return;
    }

    // Find the EF in the file
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // Look for EF marker 'E' followed by matching FID
    char ef_tag;
    uint8_t high, low;
    long pos = 0;

    while (pos < file_size) {
        pos = ftell(fp);
        if (fread(&ef_tag, sizeof(char), 1, fp) != 1) break;

        if (ef_tag == 'E') {
            // Found an EF tag, check if it's our EF
            if (fread(&high, sizeof(uint8_t), 1, fp) != 1 ||
                fread(&low, sizeof(uint8_t), 1, fp) != 1) break;

            uint16_t found_fid = (high << 8) | low;

            if (found_fid == ef->id) {
                // Found our EF, now skip size and type to get to data
                int ef_size;
                uint8_t type_byte;
                uint16_t parent_fid;

                if (fread(&ef_size, sizeof(int), 1, fp) != 1 ||
                    fread(&type_byte, sizeof(uint8_t), 1, fp) != 1 ||
                    fread(&parent_fid, sizeof(uint16_t), 1, fp) != 1) break;

                // Position is now at the start of EF data
                long data_pos = ftell(fp);

                // Seek to the offset position
                fseek(fp, data_pos + offset, SEEK_SET);

                // Write the new data
                fwrite(data, sizeof(char), length, fp);

                printf("Data written to EF 0x%04X at offset %d, length %d\n",
                    ef->id, offset, length);
                fclose(fp);
                return;
            }

            // Skip remaining EF data
            int skip_size;
            if (fread(&skip_size, sizeof(int), 1, fp) != 1) break;

            // Skip type byte and parent FID
            fseek(fp, sizeof(uint8_t) + sizeof(uint16_t), SEEK_CUR);

            // Skip the data part
            fseek(fp, skip_size, SEEK_CUR);
        }
        else {
            // Not an EF marker, continue search
            fseek(fp, 1, SEEK_CUR);
        }
    }

    printf("Could not find EF 0x%04X in the binary file\n", ef->id);
    fclose(fp);
}

// Helper function to read data from an EF
void read_ef_data(uint16_t fid) {
    File* ef = find_by_fid(MF, fid);
    if (!ef || ef->type != FILE_EF) {
        printf("Cannot read: File 0x%04X not found or not an EF.\n", fid);
        return;
    }

    printf("Data in EF 0x%04X: ", fid);
    if (ef->data && ef->data[0] != '\0') {
        printf("%s\n", ef->data);
    }
    else {
        printf("(empty)\n");
    }
}

// Function to update data in an EF
void update_data_cmd(uint16_t fid, const char* data) {
    File* ef = find_by_fid(MF, fid);
    if (!ef || ef->type != FILE_EF) {
        printf("Cannot update: File 0x%04X not found or not an EF.\n", fid);
        return;
    }

    int data_len = strlen(data);
    if (data_len > ef->size) {
        printf("Data too large for EF 0x%04X (max: %d bytes)\n", fid, ef->size);
        return;
    }

    // Update data in memory and in file
    write_data_to_ef(ef, data, 0, data_len);
    printf("Updated data in EF 0x%04X\n", fid);
}

// Function to free all allocated memory
void cleanup_memory() {
    if (!MF) return;

    // Create queue for breadth-first traversal
    File* queue[100];
    int front = 0, rear = 0;

    queue[rear++] = MF;

    while (front < rear) {
        File* current = queue[front++];

        // Add all children to queue
        for (int i = 0; i < current->childCount; i++) {
            if (current->children[i]) {
                queue[rear++] = current->children[i];
            }
        }

        // Free data
        if (current->data) {
            free(current->data);
        }

        // Free the file itself
        free(current);
    }

    MF = NULL;
    current_dir = NULL;
    used_size = 0;
}

// ---------- Main ----------
int main() {
    printf("APDU Simulator\n");

    // Try to load existing file structure
    if (load_file_structure()) {
        printf("Successfully loaded file structure from memory_store.bin\n");
        print_tree(MF, 0);
    }
    else {
        printf("No existing file structure found or error loading.\n");
    }

    char command[256];
    while (1) {
        printf("\nEnter APDU or command (type 'exit' to quit, 'tree' to show structure): ");

        if (!fgets(command, sizeof(command), stdin)) {
            break; // Exit on EOF (Ctrl+D or Ctrl+Z)
        }

        // Strip trailing newline
        size_t len = strlen(command);
        if (len > 0 && command[len - 1] == '\n') {
            command[--len] = '\0';
        }

        if (len == 0) continue; // Skip empty lines

        // Special commands
        if (strcmp(command, "exit") == 0) {
            break;
        }
        else if (strcmp(command, "tree") == 0) {
            print_tree(MF, 0);
            continue;
        }
        else if (strncmp(command, "read ", 5) == 0) {
            uint16_t fid;
            if (sscanf_s(command + 5, "%hx", &fid) == 1) {
                read_ef_data(fid);
            }
            else {
                printf("Invalid FID format. Use 'read XXXX' where XXXX is hex FID.\n");
            }
            continue;
        }
        else if (strncmp(command, "update ", 7) == 0) {
            char* ptr = command + 7;
            uint16_t fid;
            if (sscanf_s(ptr, "%hx", &fid) == 1) {
                // Find the space after FID
                while (*ptr && !isspace((unsigned char)*ptr)) ptr++;
                while (*ptr && isspace((unsigned char)*ptr)) ptr++;

                if (*ptr) {
                    update_data_cmd(fid, ptr);
                }
                else {
                    printf("Missing data. Use 'update XXXX data' where XXXX is hex FID.\n");
                }
            }
            else {
                printf("Invalid command format. Use 'update XXXX data'.\n");
            }
            continue;
        }

        // Process as APDU
        parse_apdu(command);
        memory_used();
    }

    printf("Final Tree Structure:\n");
    print_tree(MF, 0);

    // Clean up memory before exit
    cleanup_memory();

    return 0;
}
