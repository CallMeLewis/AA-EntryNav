// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// hmiapp/car_identity.hpp: the series of the car's coding byte, the VIN's serial number in a CDS text, the vehicle id
// made from it, the log's masking, and the lines the app writes for Flare Drive. Every series the coding can name is
// one core/sdr_builder.cpp knows, so the line the app offers is one the service reads.
#include <stdio.h>
#include <unistd.h>

#include <set>
#include <string>

#include "../core/sdr_builder.hpp"
#include "../hmiapp/car_identity.hpp"

namespace {

using namespace evo::app;

int g_failures = 0;

#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

void seriesBytes() {
    CHECK(std::string(seriesOfCoding(0x14)) == "F20");
    CHECK(std::string(seriesOfCoding(0x2a)) == "G20");
    CHECK(std::string(seriesOfCoding(0x3c)) == "J29");
    CHECK(std::string(seriesOfCoding(0x81)) == "RR31");
    CHECK(!seriesOfCoding(0x00) && !seriesOfCoding(0x05) && !seriesOfCoding(0xff) && !seriesOfCoding(0x114));
    // All 90 named codes, each once, each in the service's table.
    std::set<std::string> codes;
    for (unsigned v = 0; v < 256; ++v) {
        if (const char* code = seriesOfCoding(v)) {
            CHECK(codes.insert(code).second);
            CHECK(aa::findCarSeries(code) != nullptr);
        }
    }
    CHECK(codes.size() == 90);
}

void vins() {
    // Made up, in a VIN's form. iDrive gives {"VIN":"<the last 7>"}; a whole VIN gives the same serial number.
    char out[8];
    CHECK(vinSerialInText("{\"VIN\":\"AB12345\"}", out) && std::string(out) == "AB12345");
    CHECK(vinSerialInText("{\"VIN\":\"WBA1A11010AB12345\"}", out) && std::string(out) == "AB12345");
    CHECK(vinSerialInText("WBA1A11010AB12345", out) && std::string(out) == "AB12345");
    // A whole VIN wins over a 7-character run before it.
    CHECK(vinSerialInText("{\"XYZ1234\":\"WBA1A11010AB12345\"}", out) && std::string(out) == "AB12345");
    CHECK(!vinSerialInText("{\"VIN\":\"AB1234\"}", out) && out[0] == 0);    // 6
    CHECK(!vinSerialInText("{\"VIN\":\"AB123456\"}", out) && out[0] == 0);  // 8
    CHECK(!vinSerialInText("{\"VIN\":\"AO12345\"}", out) && out[0] == 0);   // O is never in a VIN
    CHECK(!vinSerialInText("{\"VIN\":\"ab12345\"}", out) && out[0] == 0);
    CHECK(!vinSerialInText("{\"VIN\":\"WBA1A11010AB123456\"}", out) && out[0] == 0);  // 18
    CHECK(!vinSerialInText("", out) && !vinSerialInText(nullptr, out));
}

void vehicleIds() {
    char a[37], b[37], c[37];
    vehicleIdOfSerial("AB12345", a);
    vehicleIdOfSerial("AB12345", b);
    vehicleIdOfSerial("AB12346", c);
    CHECK(aa::validVehicleId(a) && std::string(a) == b && std::string(a) != c);
    CHECK(a[14] == '5' && (a[19] == '8' || a[19] == '9' || a[19] == 'a' || a[19] == 'b'));  // version 5, RFC 4122
    CHECK(std::string(a).find("12345") == std::string::npos);
}

void masking() {
    char out[64];
    maskForLog("{\"VIN\":\"WBA1A11010AB12345\"}", out, sizeof(out));
    CHECK(std::string(out) == "{\"VIN\":\"WBA**************\"}");
    maskForLog("{\"VIN\":\"AB12345\"}", out, sizeof(out));
    CHECK(std::string(out) == "{\"VIN\":\"AB1****\"}");
    maskForLog("{\"type\":\"F20\",\"id\":7}", out, sizeof(out));
    CHECK(std::string(out) == "{\"type\":\"F20\",\"id\":7}");
    maskForLog("abcdef", out, 4);  // cut to the buffer
    CHECK(std::string(out) == "abc");
}

void carFile() {
    CarFacts f;
    CHECK(carFileText(f).empty());
    f.series = "F20";
    f.side = "right";
    f.vehicleId = "0f1e2d3c-4b5a-5978-8796-a5b4c3d2e1f0";
    const std::string text = carFileText(f);
    CHECK(text == "series=F20\nid=0f1e2d3c-4b5a-5978-8796-a5b4c3d2e1f0\nside=right\n");
    // The service reads the same lines as the vehicle file.
    char path[64];
    snprintf(path, sizeof(path), "/tmp/test-car-identity-%d", static_cast<int>(getpid()));
    FILE* file = fopen(path, "w");
    CHECK(file);
    if (file) {
        fputs(text.c_str(), file);
        fclose(file);
    }
    aa::SdrIdentity id;
    CHECK(id.loadVehicleFile(path) && id.carModel == "1 Series (F20)" && id.vehicleId == f.vehicleId &&
          id.rightHandDrive);
    unlink(path);
}

}  // namespace

int main() {
    seriesBytes();
    vins();
    vehicleIds();
    masking();
    carFile();
    if (g_failures) {
        printf("FAIL car identity: %d check(s)\n", g_failures);
        return 1;
    }
    puts(
        "PASS car identity: the series of the coding byte, the VIN's serial number in a CDS text, its vehicle id, the "
        "log's masking, the lines for Flare Drive read as the service reads them");
    return 0;
}
