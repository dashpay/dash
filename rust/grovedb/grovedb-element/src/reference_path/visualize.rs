use std::{fmt, io::Write};

use grovedb_visualize::{Drawer, Visualize};

use crate::{reference_path::ReferencePathType, visualize_helpers::visualize_to_vec};

impl Visualize for ReferencePathType {
    fn visualize<W: Write>(&self, mut drawer: Drawer<W>) -> std::io::Result<Drawer<W>> {
        match self {
            ReferencePathType::AbsolutePathReference(path) => {
                drawer.write(b"absolute path reference: ")?;
                drawer.write(
                    path.iter()
                        .map(hex::encode)
                        .collect::<Vec<String>>()
                        .join("/")
                        .as_bytes(),
                )?;
            }
            ReferencePathType::UpstreamRootHeightReference(height, end_path) => {
                drawer.write(b"upstream root height reference: ")?;
                drawer.write(format!("[height: {height}]").as_bytes())?;
                drawer.write(
                    end_path
                        .iter()
                        .map(hex::encode)
                        .collect::<Vec<String>>()
                        .join("/")
                        .as_bytes(),
                )?;
            }
            ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(
                height,
                end_path,
            ) => {
                drawer.write(b"upstream root height with parent path addition reference: ")?;
                drawer.write(format!("[height: {height}]").as_bytes())?;
                drawer.write(
                    end_path
                        .iter()
                        .map(hex::encode)
                        .collect::<Vec<String>>()
                        .join("/")
                        .as_bytes(),
                )?;
            }
            ReferencePathType::UpstreamFromElementHeightReference(up, end_path) => {
                drawer.write(b"upstream from element reference: ")?;
                drawer.write(format!("[up: {up}]").as_bytes())?;
                drawer.write(
                    end_path
                        .iter()
                        .map(hex::encode)
                        .collect::<Vec<String>>()
                        .join("/")
                        .as_bytes(),
                )?;
            }
            ReferencePathType::CousinReference(key) => {
                drawer.write(b"cousin reference: ")?;
                drawer = key.visualize(drawer)?;
            }
            ReferencePathType::RemovedCousinReference(middle_path) => {
                drawer.write(b"removed cousin reference: ")?;
                drawer.write(
                    middle_path
                        .iter()
                        .map(hex::encode)
                        .collect::<Vec<String>>()
                        .join("/")
                        .as_bytes(),
                )?;
            }
            ReferencePathType::SiblingReference(key) => {
                drawer.write(b"sibling reference: ")?;
                drawer = key.visualize(drawer)?;
            }
        }
        Ok(drawer)
    }
}

impl fmt::Debug for ReferencePathType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut v = Vec::new();
        visualize_to_vec(&mut v, self);

        f.write_str(&String::from_utf8_lossy(&v))
    }
}
