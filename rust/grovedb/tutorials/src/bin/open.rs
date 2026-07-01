use grovedb::GroveDb;

fn main() {
    // Specify the path where you want to set up the GroveDB instance
    let path = String::from("../tutorial-storage");

    // Open a new GroveDB at the path
    GroveDb::open(&path).unwrap();

    // Print to the terminal
    println!("Opened {:?}", path);
}
