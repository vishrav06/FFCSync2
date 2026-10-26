// Core data types shared by every part of FFCSync.
//
// Two kinds of data exist:
//   * Catalog            - loaded once at start-up and never modified afterwards.
//                          Threads may read it concurrently without locking.
//   * RegistrationState  - the mutable registration state (seats, enrolments,
//                          credits, timetable). Only the registration engine
//                          changes it, and in the server only inside the
//                          ordered commit stage.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ffcsync {

using StudentId = std::string;
using SectionId = std::string;
using CourseCode = std::string;
using Slot = std::string;     // VIT-style timetable slot, e.g. "A1", "TA1", "L31"
using Bucket = std::string;   // course bucket, e.g. "PC", "PE", "UE"

// Credits are stored in tenths of a credit (4.0 credits == 40) so that all
// limit comparisons are exact integer comparisons.
using CreditTenths = int;

enum class Op { Add, Drop };

enum class Status { Accepted, Rejected };

// Rejection reasons. The registration rules are evaluated in this order and
// the first failing rule is reported (see README.md).
enum class Reason {
    Ok,
    Malformed,
    WindowClosed,
    UnknownStudent,
    UnknownSection,
    PrereqNotMet,
    AlreadyRegistered,
    CourseAlreadyTaken,
    SectionFull,
    TimetableClash,
    CreditLimit,
    BucketLimit,
    NotRegistered,
};

std::string_view to_string(Op op);
std::string_view to_string(Status status);
std::string_view to_string(Reason reason);
std::optional<Op> op_from_string(std::string_view s);
std::optional<Status> status_from_string(std::string_view s);
std::optional<Reason> reason_from_string(std::string_view s);

struct Student {
    StudentId id;
    std::string name;
    std::set<CourseCode> completed;   // courses already passed (for prerequisites)
};

struct Course {
    CourseCode code;
    std::string title;
    CreditTenths credits = 0;
    Bucket bucket;
    std::vector<CourseCode> prereqs;
};

struct Section {
    SectionId id;
    CourseCode course;
    std::string faculty;
    std::vector<Slot> slots;
    int capacity = 0;
};

struct Config {
    bool window_open = true;
    CreditTenths max_credits = 270;
    std::map<Bucket, CreditTenths> bucket_limits;   // bucket absent => unlimited
};

// Immutable after loading.
struct Catalog {
    Config config;
    std::map<StudentId, Student> students;
    std::map<CourseCode, Course> courses;
    std::map<SectionId, Section> sections;

    const Student* find_student(const StudentId& id) const;
    const Section* find_section(const SectionId& id) const;
    const Course* find_course(const CourseCode& code) const;
};

struct RegistrationRequest {
    std::string request_id;   // chosen by the client; echoed back in the result
    Op op = Op::Add;
    StudentId student_id;
    SectionId section_id;
};

struct RegistrationResult {
    std::uint64_t seq = 0;    // FCFS sequence number (0 = never admitted)
    RegistrationRequest request;
    Status status = Status::Rejected;
    Reason reason = Reason::Malformed;
};

// Per-student mutable state. credits/bucket_credits/slots are derived from
// `sections` but stored explicitly so the invariant checker can verify that
// the engine keeps them consistent.
struct StudentState {
    std::set<SectionId> sections;
    CreditTenths credits = 0;
    std::map<Bucket, CreditTenths> bucket_credits;
    std::set<Slot> slots;

    bool operator==(const StudentState&) const = default;
};

struct RegistrationState {
    std::map<SectionId, int> enrolled;            // seats taken per section
    std::map<StudentId, StudentState> students;

    // Empty state: every section with 0 seats taken, every student unregistered.
    static RegistrationState empty_for(const Catalog& catalog);

    bool operator==(const RegistrationState&) const = default;
};

}  // namespace ffcsync
